#include "process_options.h"
#include "tone_curve_utils.h"
#include <iostream>
#include <algorithm>

void print_usage() {
    printf("Usage: ./process --input <raw.png> --output <out.png> [options]\n\n"
           "This executable is compiled for a specific precision. Run 'process_f32' or 'process_u16'.\n"
           "The 'rawr' executable provides a graphical user interface.\n\n"
           "Required arguments for command-line processing:\n"
           "  --input <path>         Path to the input 16-bit RAW PNG file.\n"
           "  --output <path>        Path for the output 8-bit image file.\n\n"
           "Pipeline Options:\n"
           "  --demosaic <name>      Demosaic algorithm. 'fast', 'ahd', 'lmmse', or 'ri' (default: fast).\n"
           "  --downscale <factor>   Downscale image by this factor (e.g., 2.0 for half size). 1.0=off (default: 1.0).\n"
           "  --exposure <stops>     Exposure compensation in stops, e.g. -1.0, 0.5, 2.0 (default: 0.0).\n"
           "  --color-temp <K>       Color temperature in Kelvin (default: 3700).\n"
           "  --tint <val>           Green/Magenta tint. >0 -> magenta, <0 -> green (default: 0.0).\n"
           "  --ca-strength <val>    Chromatic aberration correction strength. 0=off (default: 0.0).\n"
           "  --iterations <n>       Number of timing iterations for benchmark (default: 5).\n\n"
           "Denoise Options (Radius is fixed at 2.0):\n"
           "  --denoise-strength <val> Denoise strength, 0-100 (default: 50.0).\n"
           "  --denoise-eps <val>      Denoise filter epsilon (default: 0.01).\n\n"
           "Local Adjustment Options (Laplacian Pyramid):\n"
           "  --ll-detail <val>      Local detail enhancement, -100 to 100 (default: 0).\n"
           "  --ll-clarity <val>     Local clarity (mid-tone contrast), -100 to 100 (default: 0).\n"
           "  --ll-shadows <val>     Shadow recovery, -100 to 100 (default: 0).\n"
           "  --ll-highlights <val>  Highlight recovery, -100 to 100 (default: 0).\n"
           "  --ll-blacks <val>      Adjust black point, -100 to 100 (default: 0).\n"
           "  --ll-whites <val>      Adjust white point, -100 to 100 (default: 0).\n\n"
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


ProcessConfig parse_args(int argc, char **argv) {
    ProcessConfig cfg;

    if (argc == 1) {
        return cfg; // Return default config for UI if no args
    }

    // For the UI, the first positional argument is the input file
    if (argc > 1 && argv[1][0] != '-') {
        cfg.input_path = argv[1];
    }
    
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage();
            exit(0);
        }
        if (arg.rfind("--", 0) == 0) {
            if (i + 1 < argc) {
                args[arg.substr(2)] = argv[++i];
            } else {
                throw std::runtime_error("Missing value for argument " + arg);
            }
        } else {
            // Assume it's the input file if not already set
            if (cfg.input_path.empty()) {
                cfg.input_path = arg;
            }
        }
    }

    try {
        if (args.count("input")) cfg.input_path = args["input"];
        if (args.count("output")) cfg.output_path = args["output"];
        if (args.count("demosaic")) cfg.demosaic_algorithm = args["demosaic"];
        if (args.count("downscale")) cfg.downscale_factor = std::stof(args["downscale"]);
        if (args.count("exposure")) cfg.exposure = std::stof(args["exposure"]);
        if (args.count("color-temp")) cfg.color_temp = std::stof(args["color-temp"]);
        if (args.count("tint")) cfg.tint = std::stof(args["tint"]);
        if (args.count("gamma")) cfg.gamma = std::stof(args["gamma"]);
        if (args.count("contrast")) cfg.contrast = std::stof(args["contrast"]);
        if (args.count("ca-strength")) cfg.ca_strength = std::stof(args["ca-strength"]);
        if (args.count("iterations")) cfg.timing_iterations = std::stoi(args["iterations"]);
        if (args.count("denoise-strength")) cfg.denoise_strength = std::stof(args["denoise-strength"]);
        if (args.count("denoise-eps")) cfg.denoise_eps = std::stof(args["denoise-eps"]);
        
        // --- Curve Parsing ---
        // Parse strings immediately into the vector<Point> representation.
        if (args.count("curve-points")) {
            ToneCurveUtils::parse_curve_points(args["curve-points"], cfg.curve_points_global);
        }
        if (args.count("curve-r")) {
            ToneCurveUtils::parse_curve_points(args["curve-r"], cfg.curve_points_r);
        }
        if (args.count("curve-g")) {
            ToneCurveUtils::parse_curve_points(args["curve-g"], cfg.curve_points_g);
        }
        if (args.count("curve-b")) {
            ToneCurveUtils::parse_curve_points(args["curve-b"], cfg.curve_points_b);
        }

        if (args.count("curve-mode")) {
            if (args["curve-mode"] == "luma") cfg.curve_mode = 0;
            else if (args["curve-mode"] == "rgb") cfg.curve_mode = 1;
            else { std::cerr << "Warning: unknown curve-mode '" << args["curve-mode"] << "'. Defaulting to rgb.\n"; }
        }

        if (args.count("tonemap")) {
            std::string algo = args["tonemap"];
            std::transform(algo.begin(), algo.end(), algo.begin(), ::tolower);
            if (algo == "linear") cfg.tonemap_algorithm = 0;
            else if (algo == "reinhard") cfg.tonemap_algorithm = 1;
            else if (algo == "filmic") cfg.tonemap_algorithm = 2;
            else if (algo == "gamma") cfg.tonemap_algorithm = 3;
            else { std::cerr << "Warning: unknown tonemap '" << args["tonemap"] << "'. Defaulting to gamma.\n"; }
        }

        // Local Laplacian arguments
        if (args.count("ll-detail")) cfg.ll_detail = std::stof(args["ll-detail"]);
        if (args.count("ll-clarity")) cfg.ll_clarity = std::stof(args["ll-clarity"]);
        if (args.count("ll-shadows")) cfg.ll_shadows = std::stof(args["ll-shadows"]);
        if (args.count("ll-highlights")) cfg.ll_highlights = std::stof(args["ll-highlights"]);
        if (args.count("ll-blacks")) cfg.ll_blacks = std::stof(args["ll-blacks"]);
        if (args.count("ll-whites")) cfg.ll_whites = std::stof(args["ll-whites"]);

    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Error parsing arguments: ") + e.what());
    }

    return cfg;
}
