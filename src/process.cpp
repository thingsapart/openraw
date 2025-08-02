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
#include <vector>
#include <stdexcept>
#include <algorithm>
#include <cmath>

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
    int saturation_algorithm = 1; // 0=HSL, 1=LAB
    float gamma = 2.2f;
    float contrast = 50.0f;
    float black_point = 25.0f;
    float white_point = 1023.0f;
    std::string curve_points_str;
    std::string curve_r_str;
    std::string curve_g_str;
    std::string curve_b_str;
    int curve_mode = 1; // 0=Luma, 1=RGB
    float sharpen = 1.0f;
    float ca_strength = 0.0f; // Default to off
    int timing_iterations = 5;
};

struct Point { float x, y; };

// Helper to perform Catmull-Rom spline interpolation between p1 and p2.
// The points p0 and p3 are the control points before and after the segment.
// t is the interpolation factor from 0 to 1.
float interpolate_catmull_rom(const Point& p0, const Point& p1, const Point& p2, const Point& p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;

    float y = 0.5f * ( (2.0f * p1.y) +
                       (-p0.y + p2.y) * t +
                       (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 +
                       (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3 );
    return y;
}


// Helper to parse a string like "0:0,0.5:0.6,1:1" into a vector of points.
bool parse_curve_points(const std::string& s, std::vector<Point>& points) {
    if (s.empty()) return false;
    points.clear();
    std::string current_s = s;
    size_t pos = 0;
    while ((pos = current_s.find(',')) != std::string::npos) {
        std::string token = current_s.substr(0, pos);
        size_t colon_pos = token.find(':');
        if (colon_pos == std::string::npos) return false;
        try {
            float x = std::stof(token.substr(0, colon_pos));
            float y = std::stof(token.substr(colon_pos + 1));
            points.push_back({x, y});
        } catch (...) { return false; }
        current_s.erase(0, pos + 1);
    }
    // Handle the last pair
    size_t colon_pos = current_s.find(':');
    if (colon_pos == std::string::npos) return false;
    try {
        float x = std::stof(current_s.substr(0, colon_pos));
        float y = std::stof(current_s.substr(colon_pos + 1));
        points.push_back({x, y});
    } catch (...) { return false; }

    // Ensure points are sorted by x
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return a.x < b.x;
    });
    return true;
}

// Generate one column of the tone curve LUT
void generate_lut_channel(const std::vector<Point>& user_points, float black_point, float white_point, float gamma, float contrast, uint8_t* lut_col, int lut_size) {
    std::vector<Point> points;
    bool useCustomCurve = !user_points.empty();
    
    if (useCustomCurve) {
        // Add implicit 0,0 and 1,1 points if they are not defined by the user.
        points.push_back({0.0f, 0.0f});
        points.insert(points.end(), user_points.begin(), user_points.end());
        points.push_back({1.0f, 1.0f});

        // Remove duplicate points by only keeping the last one
        points.erase(std::unique(points.begin(), points.end(), [](const Point& a, const Point& b){ return a.x == b.x; }), points.end());
        std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
    }

    float inv_range = 1.0f / (white_point - black_point);

    for (int i = 0; i < lut_size; ++i) {
        float val_norm; // Value normalized to 0-1
        if (i <= black_point) val_norm = 0.0f;
        else if (i >= white_point) val_norm = 1.0f;
        else val_norm = (i - black_point) * inv_range;
        
        float mapped_val;
        if (useCustomCurve) {
            // Find the segment our value falls into
            int k = 0;
            while (k < points.size() - 1 && points[k+1].x < val_norm) {
                k++;
            }

            // Get the 4 points needed for Catmull-Rom spline
            // Handle endpoints by creating "phantom" points
            const Point& p1 = points[k];
            const Point& p2 = (k + 1 < points.size()) ? points[k+1] : p1;
            const Point& p0 = (k > 0) ? points[k-1] : Point{2*p1.x - p2.x, 2*p1.y - p2.y};
            const Point& p3 = (k + 2 < points.size()) ? points[k+2] : Point{2*p2.x - p1.x, 2*p2.y - p1.y};

            float t = 0.0f;
            if (p2.x > p1.x) {
                t = (val_norm - p1.x) / (p2.x - p1.x);
            }
            mapped_val = interpolate_catmull_rom(p0, p1, p2, p3, t);

        } else {
            // S-curve from gamma/contrast
            float b = 2.0f - pow(2.0f, contrast / 100.0f);
            float a = 2.0f - 2.0f * b;
            float g = pow(val_norm, 1.0f / gamma);
            if (g > 0.5f) mapped_val = 1.0f - (a * (1.0f - g) * (1.0f - g) + b * (1.0f - g));
            else mapped_val = a * g * g + b * g;
        }
        lut_col[i] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, mapped_val * 255.0f + 0.5f)));
    }
}


// Generate the tone curve LUT from either control points or gamma/contrast
void generate_tone_curves(const ProcessConfig& cfg, Halide::Runtime::Buffer<uint8_t, 2>& lut) {
    std::vector<Point> global_pts, r_pts, g_pts, b_pts;

    bool has_global_curve = parse_curve_points(cfg.curve_points_str, global_pts);
    bool has_r_curve = parse_curve_points(cfg.curve_r_str, r_pts);
    bool has_g_curve = parse_curve_points(cfg.curve_g_str, g_pts);
    bool has_b_curve = parse_curve_points(cfg.curve_b_str, b_pts);
    
    // Use the specific channel curve if provided, otherwise fall back to global, otherwise use gamma/contrast.
    generate_lut_channel(has_r_curve ? r_pts : (has_global_curve ? global_pts : std::vector<Point>()), cfg.black_point, cfg.white_point, cfg.gamma, cfg.contrast, &lut(0, 0), lut.width());
    generate_lut_channel(has_g_curve ? g_pts : (has_global_curve ? global_pts : std::vector<Point>()), cfg.black_point, cfg.white_point, cfg.gamma, cfg.contrast, &lut(0, 1), lut.width());
    generate_lut_channel(has_b_curve ? b_pts : (has_global_curve ? global_pts : std::vector<Point>()), cfg.black_point, cfg.white_point, cfg.gamma, cfg.contrast, &lut(0, 2), lut.width());
}


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

    // Generate the tone curve LUT on the host side
    Buffer<uint8_t, 2> tone_curves_buf(4096, 3);
    generate_tone_curves(cfg, tone_curves_buf);

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

    printf("Success!\n");
    return 0;
}
