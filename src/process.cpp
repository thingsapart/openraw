#include "halide_benchmark.h"

#include "camera_pipe.h"
#ifndef NO_AUTO_SCHEDULE
#include "camera_pipe_auto_schedule.h"
#endif

#include "HalideBuffer.h"
#include "halide_image_io.h"
#include "halide_malloc_trace.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <map>
#include <stdexcept>

using namespace Halide::Runtime;
using namespace Halide::Tools;

// A struct to hold all pipeline parameters
struct ProcessConfig {
    std::string input_path;
    std::string output_path;
    float color_temp = 3700.0f;
    float tint = 0.0f;
    float exposure = 1.0f;
    float saturation = 1.0f;
    float gamma = 2.2f;
    float contrast = 50.0f;
    float sharpen = 1.0f;
    float ca_strength = 0.0f; // Default to off
    int timing_iterations = 5;
};

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
           "  --gamma <val>          Gamma correction value (default: 2.2).\n"
           "  --contrast <val>       Contrast enhancement value (default: 50.0).\n"
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
        if (args.count("gamma")) cfg.gamma = std::stof(args["gamma"]);
        if (args.count("contrast")) cfg.contrast = std::stof(args["contrast"]);
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
    // --- End Argument Parsing ---

#ifdef HL_MEMINFO
    halide_enable_malloc_trace();
#endif

    fprintf(stderr, "input: %s\n", cfg.input_path.c_str());
    Buffer<uint16_t, 2> input = load_and_convert_image(cfg.input_path);
    fprintf(stderr, "       %d %d\n", input.width(), input.height());
    Buffer<uint8_t, 3> output(((input.width() - 32) / 32) * 32, ((input.height() - 24) / 32) * 32, 3);

#ifdef HL_MEMINFO
    info(input, "input");
    stats(input, "input");
    // dump(input, "input");
#endif

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

    int blackLevel = 25;
    int whiteLevel = 1023;

    double best;

#ifdef BENCHMARK
    best = benchmark(cfg.timing_iterations, 1, [&]() {
        camera_pipe(input, matrix_3200, matrix_7000,
                    cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.gamma, cfg.contrast, cfg.sharpen, cfg.ca_strength, blackLevel, whiteLevel,
                    output);
        output.device_sync();
    });
    fprintf(stderr, "Halide (manual):\t%gus\n", best * 1e6);
#else
    camera_pipe(input, matrix_3200, matrix_7000,
                cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.gamma, cfg.contrast, cfg.sharpen, cfg.ca_strength, blackLevel, whiteLevel,
                output);
    output.device_sync();
#endif

#define NO_AUTO_SCHEDULE

#ifndef NO_AUTO_SCHEDULE
#ifdef BENCHMARK
    best = benchmark(cfg.timing_iterations, 1, [&]() {
        camera_pipe_auto_schedule(input, matrix_3200, matrix_7000,
                                  cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.gamma, cfg.contrast, cfg.sharpen, cfg.ca_strength, blackLevel, whiteLevel,
                                  output);
        output.device_sync();
    });
    fprintf(stderr, "Halide (auto):\t%gus\n", best * 1e6);
#else
    camera_pipe_auto_schedule(input, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, cfg.exposure, cfg.saturation, cfg.gamma, cfg.contrast, cfg.sharpen, cfg.ca_strength, blackLevel, whiteLevel,
                              output);
    output.device_sync();
#endif
#endif

    fprintf(stderr, "output: %s\n", cfg.output_path.c_str());
    convert_and_save_image(output, cfg.output_path);
    fprintf(stderr, "        %d %d\n", output.width(), output.height());

    printf("Success!\n");
    return 0;
}
