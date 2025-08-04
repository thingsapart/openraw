#include <cstdint>
#include <string>
#include <map>
#include <stdexcept>
#include <iostream>
#include <cstdlib> // For getenv
#include <chrono>  // For timing
#include <limits>  // For numeric_limits

#include "HalideBuffer.h"
#include "halide_image_io.h"
#include "halide_malloc_trace.h"
#include "tone_curve_utils.h"

// Conditionally include the generated pipeline headers based on the
// macro defined by CMake.
#if defined(PIPELINE_PRECISION_F32)
#include "camera_pipe_f32_lib.h"
#elif defined(PIPELINE_PRECISION_U16)
#include "camera_pipe_u16_lib.h"
#else
#error "PIPELINE_PRECISION_F32 or PIPELINE_PRECISION_U16 must be defined"
#endif

// By default, the auto-scheduled pipeline is not compiled or run.
#define NO_AUTO_SCHEDULE
#ifndef NO_AUTO_SCHEDULE
    #if defined(PIPELINE_PRECISION_F32)
    #include "camera_pipe_f32_auto_schedule_lib.h"
    #elif defined(PIPELINE_PRECISION_U16)
    #include "camera_pipe_u16_auto_schedule_lib.h"
    #endif
#endif

using namespace Halide::Runtime;
using namespace Halide::Tools;

void print_usage() {
    printf("Usage: ./process --input <raw.png> --output <out.png> [options]\n\n"
           "This executable is compiled for a specific precision. Run 'process_f32' or 'process_u16'.\n\n"
           "Required arguments:\n"
           "  --input <path>         Path to the input 16-bit RAW PNG file.\n"
           "  --output <path>        Path for the output 8-bit image file.\n\n"
           "Pipeline Options:\n"
           "  --demosaic <name>      Demosaic algorithm. 'fast', 'ahd', 'lmmse', or 'ri' (default: fast).\n"
           "  --color-temp <K>       Color temperature in Kelvin (default: 3700).\n"
           "  --tint <val>           Green/Magenta tint. >0 -> magenta, <0 -> green (default: 0.0).\n"
           "  --sharpen <val>        Sharpening strength (default: 1.0).\n"
           "  --ca-strength <val>    Chromatic aberration correction strength. 0=off (default: 0.0).\n"
           "  --iterations <n>       Number of timing iterations for benchmark (default: 5).\n\n"
           "Denoise Options:\n"
           "  --denoise-strength <val> Denoise strength, 0-100 (default: 50.0).\n"
           "  --denoise-radius <val>   Denoise filter radius in pixels (default: 2.0).\n"
           "  --denoise-eps <val>      Denoise filter epsilon (default: 0.01).\n\n"
           "Tone Mapping Options (These are mutually exclusive; curves override others):\n"
           "  --tonemap <name>       Global tonemap operator. 'linear', 'reinhard', 'filmic', 'gamma' (default).\n"
           "  --gamma <val>          Gamma correction value (default: 2.2). Used if no curve is given.\n"
           "  --contrast <val>       Contrast enhancement value (default: 50.0). Used if no curve is given.\n"
           "  --curve-points <str>   Global curve points, e.g. \"0:0,0.5:0.4,1:1\". Overrides gamma/contrast.\n"
           "  --curve-r <str>        Red channel curve points. Overrides --curve-points for red.\n"
           "  --curve-g <str>        Green channel curve points. Overrides --curve-points for green.\n"
           "  --curve-b <str>        Blue channel curve points. Overrides --curve-points for blue.\n"
           "  --curve-mode <name>    Curve mode. 'luma' or 'rgb' (default: rgb).\n\n"
           "  --help                 Display this help message.\n");
}

int main(int argc, char **argv) {
    if (argc == 1) {
        print_usage();
        return 0;
    }

    // --- Argument Parsing ---
    ProcessConfig cfg;
    int demosaic_id = 3; // 0=ahd, 1=lmmse, 2=ri, 3=fast

    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(); return 0; }
        if (arg.rfind("--", 0) == 0) {
            if (i + 1 < argc) { args[arg.substr(2)] = argv[++i]; }
            else { fprintf(stderr, "Error: Missing value for argument %s\n", arg.c_str()); return 1; }
        } else {
            fprintf(stderr, "Error: Unrecognized positional argument: %s\n", arg.c_str());
            print_usage();
            return 1;
        }
    }

    try {
        if (args.count("input")) cfg.input_path = args["input"];
        if (args.count("output")) cfg.output_path = args["output"];
        if (args.count("demosaic")) {
            if (args["demosaic"] == "ahd") demosaic_id = 0;
            else if (args["demosaic"] == "lmmse") demosaic_id = 1;
            else if (args["demosaic"] == "ri") demosaic_id = 2;
            else if (args["demosaic"] == "fast") demosaic_id = 3;
            else { std::cerr << "Warning: unknown demosaic algorithm '" << args["demosaic"] << "'. Defaulting to fast.\n"; }
        }
        if (args.count("color-temp")) cfg.color_temp = std::stof(args["color-temp"]);
        if (args.count("tint")) cfg.tint = std::stof(args["tint"]);
        if (args.count("gamma")) cfg.gamma = std::stof(args["gamma"]);
        if (args.count("contrast")) cfg.contrast = std::stof(args["contrast"]);
        if (args.count("sharpen")) cfg.sharpen = std::stof(args["sharpen"]);
        if (args.count("ca-strength")) cfg.ca_strength = std::stof(args["ca-strength"]);
        if (args.count("iterations")) cfg.timing_iterations = std::stoi(args["iterations"]);
        if (args.count("denoise-strength")) cfg.denoise_strength = std::stof(args["denoise-strength"]);
        if (args.count("denoise-radius")) cfg.denoise_radius = std::stof(args["denoise-radius"]);
        if (args.count("denoise-eps")) cfg.denoise_eps = std::stof(args["denoise-eps"]);
        if (args.count("curve-points")) cfg.curve_points_str = args["curve-points"];
        if (args.count("curve-r")) cfg.curve_r_str = args["curve-r"];
        if (args.count("curve-g")) cfg.curve_g_str = args["curve-g"];
        if (args.count("curve-b")) cfg.curve_b_str = args["curve-b"];
        if (args.count("curve-mode")) {
            if (args["curve-mode"] == "luma") cfg.curve_mode = 0;
            else if (args["curve-mode"] == "rgb") cfg.curve_mode = 1;
            else { std::cerr << "Warning: unknown curve-mode '" << args["curve-mode"] << "'. Defaulting to rgb.\n"; }
        }
        if (args.count("tonemap")) {
            if (args["tonemap"] == "linear") cfg.tonemap_algorithm = 0;
            else if (args["tonemap"] == "reinhard") cfg.tonemap_algorithm = 1;
            else if (args["tonemap"] == "filmic") cfg.tonemap_algorithm = 2;
            else if (args["tonemap"] == "gamma") cfg.tonemap_algorithm = 3;
            else { std::cerr << "Warning: unknown tonemap '" << args["tonemap"] << "'. Defaulting to gamma.\n"; }
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "Error parsing arguments: %s\n", e.what()); return 1;
    }

    if (cfg.input_path.empty() || cfg.output_path.empty()) {
        fprintf(stderr, "Error: --input and --output arguments are required.\n\n");
        print_usage();
        return 1;
    }

    fprintf(stderr, "input: %s\n", cfg.input_path.c_str());
    Buffer<uint16_t, 2> input = load_and_convert_image(cfg.input_path);
    fprintf(stderr, "       %d %d\n", input.width(), input.height());
    Buffer<uint8_t, 3> output(((input.width() - 32) / 32) * 32, ((input.height() - 24) / 32) * 32, 3);

    ToneCurveUtils tone_curve_util(cfg);
    Buffer<uint16_t, 2> tone_curve_lut = tone_curve_util.get_lut_for_halide();

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

    int blackLevel = 25;
    int whiteLevel = 1023;
    float denoise_strength_norm = std::max(0.0f, std::min(1.0f, cfg.denoise_strength / 100.0f));

    // --- Simple Timing/Profiling Loop ---
    double best_time = std::numeric_limits<double>::infinity();
    for (int i = 0; i < cfg.timing_iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        #if defined(PIPELINE_PRECISION_F32)
            camera_pipe_f32(input, demosaic_id, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, cfg.sharpen, cfg.ca_strength,
                              denoise_strength_norm, cfg.denoise_radius, cfg.denoise_eps,
                              blackLevel, whiteLevel, tone_curve_lut, output);
        #elif defined(PIPELINE_PRECISION_U16)
            camera_pipe_u16(input, demosaic_id, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, cfg.sharpen, cfg.ca_strength,
                              denoise_strength_norm, cfg.denoise_radius, cfg.denoise_eps,
                              blackLevel, whiteLevel, tone_curve_lut, output);
        #endif
        output.device_sync();

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        if (elapsed.count() < best_time) {
            best_time = elapsed.count();
        }
    }

    // The profiler will print its report to stderr automatically when the program
    // exits, so we just need to print our own timing results.
    // Check if profiling was enabled to avoid printing benchmark time when it's not relevant.
    if (getenv("HL_PROFILE") == nullptr) {
        #if defined(PIPELINE_PRECISION_F32)
            fprintf(stderr, "Using float32 pipeline.\n");
        #elif defined(PIPELINE_PRECISION_U16)
            fprintf(stderr, "Using uint16_t pipeline.\n");
        #endif
        fprintf(stderr, "Halide (manual):\t%gus\n", best_time * 1e6);
    }

#ifndef NO_AUTO_SCHEDULE
    // Auto-schedule benchmarking would go here, with a similar if/else structure
#endif

    fprintf(stderr, "output: %s\n", cfg.output_path.c_str());
    convert_and_save_image(output, cfg.output_path);
    fprintf(stderr, "        %d %d\n", output.width(), output.height());

    std::string curve_png_path = cfg.output_path.substr(0, cfg.output_path.find_last_of('.')) + "_curve.png";
    if (tone_curve_util.render_curves_to_png(curve_png_path.c_str())) {
        fprintf(stderr, "curve:  %s\n", curve_png_path.c_str());
    }

    printf("Success!\n");
    return 0;
}
