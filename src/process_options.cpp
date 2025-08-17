#include "process_options.h"
#include "tone_curve_utils.h"
#include <iostream>
#include <algorithm>
#include <sstream>

// Helper to parse a string "x,y" into a Point struct
static bool parse_point(const std::string& s, Point& p) {
    std::stringstream ss(s);
    char comma;
    ss >> p.x >> comma >> p.y;
    return !ss.fail() && comma == ',';
}

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
           "  --dehaze <val>         Dehaze strength, 0-100 (default: 0.0).\n"
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
           "Color Grading Options:\n"
           "  --shadows-wheel <x,y>  Color wheel offset for shadows (e.g., \"0.1,-0.05\").\n"
           "  --shadows-luma <val>   Luminance adjustment for shadows.\n"
           "  --midtones-wheel <x,y> Color wheel offset for midtones.\n"
           "  --midtones-luma <val>  Luminance adjustment for midtones.\n"
           "  --highlights-wheel <x,y> Color wheel offset for highlights.\n"
           "  --highlights-luma <val> Luminance adjustment for highlights.\n"
           "  --h-vs-h <pts>         Hue vs Hue curve points, e.g. \"0:0,0.5:0.1,1:0\".\n"
           "  --h-vs-s <pts>         Hue vs Sat curve points.\n"
           "  --h-vs-l <pts>         Hue vs Luma curve points.\n"
           "  --l-vs-s <pts>         Luma vs Sat curve points.\n"
           "  --s-vs-s <pts>         Sat vs Sat curve points.\n\n"
           "Lens Correction Options:\n"
           "  --vignette-amount <val>    Vignette strength, -100 to 100 (default: 0).\n"
           "  --vignette-midpoint <val>  Vignette feather/reach, 0 to 100 (default: 50).\n"
           "  --vignette-roundness <val> Vignette shape, 0 (circular) to 100 (elliptical) (default: 100).\n"
           "  --vignette-highlights <val> Highlight protection, 0 to 100 (default: 0).\n\n"
           "Tone Mapping Options (These are mutually exclusive; curves override others):\n"
           "  --tonemap <name>       Global tonemap operator. 'linear', 'reinhard', 'filmic', 'gamma' (default).\n"
           "  --gamma <val>          Gamma correction value (default: 2.2). Used if no curve is given.\n"
           "  --contrast <val>       Contrast enhancement value (default: 50.0). Used if no curve is given.\n"
           "  --curve-points <str>   Global/Luma curve points, e.g. \"0:0,0.5:0.4,1:1\". Overrides gamma/contrast.\n"
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
        if (args.count("dehaze")) cfg.dehaze_strength = std::stof(args["dehaze"]);
        if (args.count("iterations")) cfg.timing_iterations = std::stoi(args["iterations"]);
        if (args.count("denoise-strength")) cfg.denoise_strength = std::stof(args["denoise-strength"]);
        if (args.count("denoise-eps")) cfg.denoise_eps = std::stof(args["denoise-eps"]);

        // --- Curve Parsing ---
        // Parse strings immediately into the vector<Point> representation.
        if (args.count("curve-points")) {
            ToneCurveUtils::parse_curve_points(args["curve-points"], cfg.curve_points_luma);
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

        // Color Grading arguments
        if (args.count("shadows-wheel")) parse_point(args["shadows-wheel"], cfg.shadows_wheel);
        if (args.count("shadows-luma")) cfg.shadows_luma = std::stof(args["shadows-luma"]);
        if (args.count("midtones-wheel")) parse_point(args["midtones-wheel"], cfg.midtones_wheel);
        if (args.count("midtones-luma")) cfg.midtones_luma = std::stof(args["midtones-luma"]);
        if (args.count("highlights-wheel")) parse_point(args["highlights-wheel"], cfg.highlights_wheel);
        if (args.count("highlights-luma")) cfg.highlights_luma = std::stof(args["highlights-luma"]);

        if (args.count("h-vs-h")) ToneCurveUtils::parse_curve_points(args["h-vs-h"], cfg.curve_hue_vs_hue);
        if (args.count("h-vs-s")) ToneCurveUtils::parse_curve_points(args["h-vs-s"], cfg.curve_hue_vs_sat);
        if (args.count("h-vs-l")) ToneCurveUtils::parse_curve_points(args["h-vs-l"], cfg.curve_hue_vs_lum);
        if (args.count("l-vs-s")) ToneCurveUtils::parse_curve_points(args["l-vs-s"], cfg.curve_lum_vs_sat);
        if (args.count("s-vs-s")) ToneCurveUtils::parse_curve_points(args["s-vs-s"], cfg.curve_sat_vs_sat);

        // Lens Correction
        if (args.count("vignette-amount")) cfg.vignette_amount = std::stof(args["vignette-amount"]);
        if (args.count("vignette-midpoint")) cfg.vignette_midpoint = std::stof(args["vignette-midpoint"]);
        if (args.count("vignette-roundness")) cfg.vignette_roundness = std::stof(args["vignette-roundness"]);
        if (args.count("vignette-highlights")) cfg.vignette_highlights = std::stof(args["vignette-highlights"]);

    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("Error parsing arguments: ") + e.what());
    }

    return cfg;
}

