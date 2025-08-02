#include "halide_benchmark.h"

#include "camera_pipe.h"
#ifndef NO_AUTO_SCHEDULE
#include "camera_pipe_auto_schedule.h"
#endif

#include "HalideBuffer.h"
#include "halide_image_io.h"
#include "halide_malloc_trace.h"
#include "tone_curve_utils.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cmath>

using namespace Halide::Runtime;
using namespace Halide::Tools;

// Prints the usage message for the process executable.
void print_usage() {
    printf("Usage: ./process --input <raw.png> --output <out.png> [options]\n\n"
           "Required arguments:\n"
           "  --input <path>         Path to the input 16-bit RAW PNG file.\n"
           "  --output <path>        Path for the output 8-bit image file.\n\n"
           "Options:\n"
           "  --color-temp <K>       Color temperature in Kelvin (default: 3700).\n"
           "  --tint <val>           Green/Magenta tint. >0 -> magenta, <0 -> green (default: 0.0).\n"
           "  --exposure <factor>    Exposure multiplier (e.g., 2.0 is +1 stop). Default: 1.0.\n"
           "  --saturation <factor>  Color saturation. 0=grayscale, 1=normal. Default: 1.0.\n"
           "  --saturation-algo <id> Saturation algorithm. 'hsl' or 'lab' (default: lab).\n"
           "  --black-point <val>    The input value that maps to black (default: 25.0).\n"
           "  --white-point <val>    The input value that maps to white (default: 1023.0).\n"
           "  --gamma <val>          Gamma for default S-curve. Default: 2.2.\n"
           "  --contrast <val>       Contrast for default S-curve. Default: 50.0.\n"
           "  --curve-points <str>   Global curve points, e.g. \"0:0,1:1\". Overrides gamma/contrast.\n"
           "  --curve-r <str>        Red channel curve points. Overrides --curve-points for red.\n"
           "  --curve-g <str>        Green channel curve points. Overrides --curve-points for green.\n"
           "  --curve-b <str>        Blue channel curve points. Overrides --curve-points for blue.\n"
           "  --curve-mode <id>      Curve mode. 'luma' or 'rgb' (default: rgb).\n"
           "  --sharpen <val>        Sharpening strength (default: 1.0).\n"
           "  --ca-strength <val>    Chromatic aberration correction strength. 0=off (default: 0.0).\n"
           "  --iterations <n>       Number of timing iterations for benchmark (default: 5).\n"
           "  --help                 Display this help message.\n");
}

int main(int argc, char **argv) {
    if (argc == 1) {
        print_usage();
        return 0;
    }

    // --- Argument Parsing ---
    ProcessConfig cfg;
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg.rfind("--", 0) == 0) {
            if (i + 1 < argc) {
                args[arg.substr(2)] = argv[++i];
            } else {
                fprintf(stderr, "Error: Missing value for argument %s\n", arg.c_str());
                return 1;
            }
        } else {
            fprintf(stderr, "Error: Unrecognized positional argument: %s\n", arg.c_str());
            print_usage();
            return 1;
        }
    }

    try {
        if (args.count("input")) cfg.input_path = args["input"];
        if (args.count("output")) cfg.output_path = args["output"];
        if (args.count("color-temp")) cfg.color_temp = std::stof(args["color-temp"]);
        if (args.count("tint")) cfg.tint = std::stof(args["tint"]);
        if (args.count("exposure")) cfg.exposure = std::stof(args["exposure"]);
        if (args.count("saturation")) cfg.saturation = std::stof(args["saturation"]);
        if (args.count("saturation-algo")) {
            std::string algo = args["saturation-algo"];
            std::transform(algo.begin(), algo.end(), algo.begin(), ::tolower);
            if (algo == "hsl") cfg.saturation_algorithm = 0;
            else if (algo == "lab") cfg.saturation_algorithm = 1;
            else { fprintf(stderr, "Error: Unknown saturation algorithm '%s'. Use 'hsl' or 'lab'.\n", args["saturation-algo"].c_str()); return 1; }
        }
        if (args.count("black-point")) cfg.black_point = std::stof(args["black-point"]);
        if (args.count("white-point")) cfg.white_point = std::stof(args["white-point"]);
        if (args.count("gamma")) cfg.gamma = std::stof(args["gamma"]);
        if (args.count("contrast")) cfg.contrast = std::stof(args["contrast"]);
        if (args.count("curve-points")) cfg.curve_points_str = args["curve-points"];
        if (args.count("curve-r")) cfg.curve_r_str = args["curve-r"];
        if (args.count("curve-g")) cfg.curve_g_str = args["curve-g"];
        if (args.count("curve-b")) cfg.curve_b_str = args["curve-b"];
        if (args.count("curve-mode")) {
            std::string mode = args["curve-mode"];
            std::transform(mode.begin(), mode.end(), mode.begin(), ::tolower);
            if (mode == "luma") cfg.curve_mode = 0;
            else if (mode == "rgb") cfg.curve_mode = 1;
            else { fprintf(stderr, "Error: Unknown curve mode '%s'. Use 'luma' or 'rgb'.\n", args["curve-mode"].c_str()); return 1; }
        }
        if (args.count("sharpen")) cfg.sharpen = std::stof(args["sharpen"]);
        if (args.count("ca-strength")) cfg.ca_strength = std::stof(args["ca-strength"]);
        if (args.count("iterations")) cfg.timing_iterations = std::stoi(args["iterations"]);
    } catch (const std::exception& e) {
        fprintf(stderr, "Error parsing arguments: %s\n", e.what());
        return 1;
    }

    if (cfg.input_path.empty() || cfg.output_path.empty()) {
        fprintf(stderr, "Error: --input and --output arguments are required.\n\n");
        print_usage();
        return 1;
    }
    // If per-channel curves are used, force RGB mode.
    if (!cfg.curve_r_str.empty() || !cfg.curve_g_str.empty() || !cfg.curve_b_str.empty()) {
        cfg.curve_mode = 1; // Force RGB mode
    }
    // --- End Argument Parsing ---

#ifdef HL_MEMINFO
    halide_enable_malloc_trace();
#endif

    fprintf(stderr, "input: %s\n", cfg.input_path.c_str());
    Buffer<uint16_t, 2> input = load_and_convert_image(cfg.input_path);
    fprintf(stderr, "       %d %d\n", input.width(), input.height());
    Buffer<uint8_t, 3> output(((input.width() - 32) / 32) * 32, ((input.height() - 24) / 32) * 32, 3);

    // Generate the tone curve LUT on the host side using the new utility class
    ToneCurveUtils curve_util(cfg);
    Buffer<uint8_t, 2> tone_curves_buf = curve_util.get_lut_for_halide();

    // These color matrices are for the sensor in the Nokia N900 and are
    // taken from the FCam source.
    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f},
                               {-0.3576f, 1.0615f, 1.5949f, -37.1158f},
                               {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};

    float _matrix_7000[][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f},
                               {-0.3826f, 1.5906f, -0.2080f, -25.4311f},
                               {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};
    Buffer<float, 2> matrix_3200(4, 3), matrix_7000(4, 3);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            matrix_3200(j, i) = _matrix_3200[i][j];
            matrix_7000(j, i) = _matrix_7000[i][j];
        }
    }

    double best;

#ifdef BENCHMARK
    best = benchmark(cfg.timing_iterations, 1, [&]() {
        camera_pipe(input, matrix_3200, matrix_7000,
                    cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.saturation_algorithm,
                    cfg.curve_mode, tone_curves_buf,
                    cfg.sharpen, cfg.ca_strength, output);
        output.device_sync();
    });
    fprintf(stderr, "Halide (manual):\t%gus\n", best * 1e6);
#else
    camera_pipe(input, matrix_3200, matrix_7000,
                cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.saturation_algorithm,
                cfg.curve_mode, tone_curves_buf,
                cfg.sharpen, cfg.ca_strength, output);
    output.device_sync();
#endif

#define NO_AUTO_SCHEDULE

#ifndef NO_AUTO_SCHEDULE
#ifdef BENCHMARK
    best = benchmark(cfg.timing_iterations, 1, [&]() {
        camera_pipe_auto_schedule(input, matrix_3200, matrix_7000,
                                  cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.saturation_algorithm,
                                  cfg.curve_mode, tone_curves_buf,
                                  cfg.sharpen, cfg.ca_strength, output);
        output.device_sync();
    });
    fprintf(stderr, "Halide (auto):\t%gus\n", best * 1e6);
#else
    camera_pipe_auto_schedule(input, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.saturation_algorithm,
                              cfg.curve_mode, tone_curves_buf,
                              cfg.sharpen, cfg.ca_strength, output);
    output.device_sync();
#endif
#endif

    fprintf(stderr, "output: %s\n", cfg.output_path.c_str());
    convert_and_save_image(output, cfg.output_path);
    fprintf(stderr, "        %d %d\n", output.width(), output.height());

    // Generate the curve visualization
    std::string base_path = cfg.output_path;
    size_t dot_pos = base_path.rfind('.');
    if (dot_pos != std::string::npos) {
        base_path = base_path.substr(0, dot_pos);
    }
    std::string curve_path = base_path + "-curves.png";
    if (curve_util.render_curves_to_png(curve_path.c_str())) {
        fprintf(stderr, "curve viz: %s\n", curve_path.c_str());
    } else {
        fprintf(stderr, "Error: Failed to write curve visualization to %s\n", curve_path.c_str());
    }

    printf("Success!\n");
    return 0;
}
