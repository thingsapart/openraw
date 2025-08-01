#include "halide_benchmark.h"

#include "camera_pipe.h"
#include "camera_pipe_auto_schedule.h"
#include "rgb_pipe.h"
#include "rgb_pipe_auto_schedule.h"

#include "HalideBuffer.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>
#include <sstream>
#include <map>

using namespace Halide::Runtime;
using namespace Halide::Tools;

struct Config {
    std::string input_path, output_path = "processed.png";
    std::string input_type = "raw";
    bool bypass_demosaic = false;
    float black_level = 64.0f;
    float sharpen_amount = 1.0f;
    float shadows = 0.0f, highlights = 0.0f, midtones = 0.0f;
    int timing_iterations = 5;
    std::vector<std::pair<float, float>> curve_points = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    bool benchmark = false;
    std::string schedule = "auto";
};

Buffer<float> generate_curve_lut(const std::vector<std::pair<float, float>>& points, int lut_size = 1024) { auto lut = Buffer<float>(lut_size); auto sorted_points = points; std::sort(sorted_points.begin(), sorted_points.end(), [](const auto& a, const auto& b){ return a.first < b.first; }); for (int i = 0; i < lut_size; ++i) { float x = (float)i / (lut_size - 1); int p_idx = 0; while (p_idx < sorted_points.size() - 1 && sorted_points[p_idx + 1].first < x) { p_idx++; } float x1 = sorted_points[p_idx].first, y1 = sorted_points[p_idx].second, x2 = sorted_points[p_idx + 1].first, y2 = sorted_points[p_idx + 1].second; float t = (x2 - x1) > 1e-6 ? (x - x1) / (x2 - x1) : 0; lut(i) = y1 * (1.0f - t) + y2 * t; } return lut; }
std::vector<std::pair<float, float>> parse_curve_points(const std::string& s) { std::vector<std::pair<float, float>> points; std::stringstream ss(s); std::string point_str; while (std::getline(ss, point_str, ';')) { std::stringstream point_ss(point_str); std::string x_str, y_str; if (std::getline(point_ss, x_str, ',') && std::getline(point_ss, y_str, ',')) { try { points.push_back({std::stof(x_str), std::stof(y_str)}); } catch (const std::exception& e) { fprintf(stderr, "Warning: Could not parse curve point: %s\n", point_str.c_str()); } } } if (points.size() < 2) { fprintf(stderr, "Warning: Invalid curve points. Using default.\n"); return {{0.0f, 0.0f}, {1.0f, 1.0f}}; } return points; }
Buffer<uint16_t, 2> load_raw_png(const std::string& filename) { int width, height, channels; uint16_t* data = stbi_load_16(filename.c_str(), &width, &height, &channels, 1); if (!data) { fprintf(stderr, "Error: Could not load RAW PNG file %s\n", filename.c_str()); exit(1); } Buffer<uint16_t, 2> buffer(width, height); memcpy(buffer.data(), data, width * height * sizeof(uint16_t)); stbi_image_free(data); return buffer; }
Buffer<uint8_t, 3> load_rgb_png(const std::string& filename) { int width, height, channels; uint8_t* data = stbi_load(filename.c_str(), &width, &height, &channels, 3); if (!data) { fprintf(stderr, "Error: Could not load RGB file %s\n", filename.c_str()); exit(1); } Buffer<uint8_t, 3> buffer(data, {width, height, 3}); buffer.set_host_dirty(); stbi_image_free(data); return buffer; }
void save_processed_png(const Buffer<uint8_t, 3>& buffer, const std::string& filename) { stbi_write_png(filename.c_str(), buffer.width(), buffer.height(), buffer.channels(), buffer.data(), buffer.width() * buffer.channels()); }

Buffer<uint16_t, 2> load_and_bayer_rgb_png(const std::string& filename) {
    int width, height, channels;
    uint8_t* rgb_data = stbi_load(filename.c_str(), &width, &height, &channels, 3);
    if (!rgb_data) { fprintf(stderr, "Error: Could not load RGB file %s\n", filename.c_str()); exit(1); }
    if (channels < 3) { fprintf(stderr, "Error: Input image must be RGB.\n"); stbi_image_free(rgb_data); exit(1); }
    Buffer<uint16_t, 2> bayer_buffer(width, height);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t* p = rgb_data + (y * width + x) * 3;
            if (y % 2 == 0) { if (x % 2 == 0) bayer_buffer(x, y) = (uint16_t)p[1] << 8; else bayer_buffer(x, y) = (uint16_t)p[2] << 8;
            } else { if (x % 2 == 0) bayer_buffer(x, y) = (uint16_t)p[1] << 8; else bayer_buffer(x, y) = (uint16_t)p[0] << 8; }
        }
    }
    stbi_image_free(rgb_data);
    return bayer_buffer;
}

void print_usage() {
    printf("Usage: ./process --input <image.png> [options]\n\n"
           "I/O Options:\n"
           "  --input <path>      Input image file (Required).\n"
           "  --input-type <raw|rgb> Type of input file. (default: raw)\n"
           "  --output <path>     Output PNG file (default: processed.png).\n"
           "  --help              Display this help message.\n\n"
           "Execution Options:\n"
           "  --bypass-demosaic   For RGB inputs, bypass demosaic and process directly.\n"
           "  --benchmark         Run in benchmark mode (off by default).\n"
           "  --schedule <auto|manual> Select schedule to run (default: auto).\n"
           "  --iterations <N>    Number of timing iterations for benchmark mode (default: 5).\n\n"
           "Pipeline Parameters:\n"
           "  --black-level <f>   Default: 64.0 (RAW mode only)\n"
           "  --shadows <f>       Boost shadows [0..1]. Default: 0.0\n"
           "  --highlights <f>    Recover highlights [0..1]. Default: 0.0\n"
           "  --midtones <f>      Adjust midtone contrast [-1..1]. Default: 0.0\n"
           "  --curve-points <s>  Semicolon-separated points \"x,y;...\". Default: \"0,0;1,1\"\n"
           "  --sharpen-amount <f> Amount of sharpening. Default: 1.0\n\n");
}

int main(int argc, char **argv) {
    Config cfg;
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(); return 0; }
        if (arg == "--benchmark") { args["benchmark"] = "true"; continue; }
        if (arg == "--bypass-demosaic") { args["bypass-demosaic"] = "true"; continue; }
        if (arg.rfind("--", 0) == 0 && i + 1 < argc) { args[arg.substr(2)] = argv[++i]; }
    }

    try {
        if (args.count("input")) cfg.input_path = args["input"]; else { fprintf(stderr, "Error: --input is required.\n"); print_usage(); return 1; }
        if (args.count("output")) cfg.output_path = args["output"];
        if (args.count("input-type")) cfg.input_type = args["input-type"];
        if (args.count("bypass-demosaic")) cfg.bypass_demosaic = true;
        if (args.count("iterations")) cfg.timing_iterations = std::stoi(args["iterations"]);
        if (args.count("benchmark")) cfg.benchmark = true;
        if (args.count("schedule")) cfg.schedule = args["schedule"];
        if (args.count("black-level")) cfg.black_level = std::stof(args["black-level"]);
        if (args.count("shadows")) cfg.shadows = std::stof(args["shadows"]);
        if (args.count("highlights")) cfg.highlights = std::stof(args["highlights"]);
        if (args.count("midtones")) cfg.midtones = std::stof(args["midtones"]);
        if (args.count("sharpen-amount")) cfg.sharpen_amount = std::stof(args["sharpen-amount"]);
        if (args.count("curve-points")) cfg.curve_points = parse_curve_points(args["curve-points"]);
    } catch (const std::exception& e) { fprintf(stderr, "Error parsing arguments: %s\n", e.what()); return 1; }
    
    auto curve_lut = generate_curve_lut(cfg.curve_points);
    bool use_auto_schedule = (cfg.schedule == "auto");
    int error = 0;

#ifdef NO_AUTO_SCHEDULE
    if (use_auto_schedule) { fprintf(stderr, "Warning: Auto-scheduled pipeline not available. Falling back to manual schedule.\n"); use_auto_schedule = false; }
#endif

    // FIX: Declare a single output buffer before the branches.
    Buffer<uint8_t, 3> output;

    if (cfg.bypass_demosaic) {
        if (cfg.input_type != "rgb") { fprintf(stderr, "Error: --bypass-demosaic can only be used with --input-type rgb.\n"); return 1; }
        fprintf(stderr, "Loading RGB input for bypass pipeline: %s\n", cfg.input_path.c_str());
        Buffer<uint8_t, 3> input = load_rgb_png(cfg.input_path);
        
        // FIX: Initialize the output buffer for this path. No cropping needed.
        output = Buffer<uint8_t, 3>(input.width(), input.height(), 3);

        if (use_auto_schedule) {
            error = rgb_pipe_auto_schedule(input, cfg.sharpen_amount, cfg.shadows, cfg.highlights, cfg.midtones, curve_lut, output);
        } else {
            error = rgb_pipe(input, cfg.sharpen_amount, cfg.shadows, cfg.highlights, cfg.midtones, curve_lut, output);
        }

    } else {
        Buffer<uint16_t, 2> input;
        if (cfg.input_type == "rgb") { fprintf(stderr, "Loading RGB input and converting to Bayer: %s\n", cfg.input_path.c_str()); input = load_and_bayer_rgb_png(cfg.input_path);
        } else { fprintf(stderr, "Loading RAW input: %s\n", cfg.input_path.c_str()); input = load_raw_png(cfg.input_path); }
        
        // FIX: Initialize the output buffer for this path. Cropping is needed.
        output = Buffer<uint8_t, 3>((input.width() - 32), (input.height() - 24), 3);
        
        Buffer<float, 2> ccm(3, 3);
        ccm.fill(0.0f); ccm(0, 0) = 1.0f; ccm(1, 1) = 1.0f; ccm(2, 2) = 1.0f;
        
        if (use_auto_schedule) {
            error = camera_pipe_auto_schedule(input, ccm, cfg.black_level, cfg.sharpen_amount, cfg.shadows, cfg.highlights, cfg.midtones, curve_lut, output);
        } else {
            error = camera_pipe(input, ccm, cfg.black_level, cfg.sharpen_amount, cfg.shadows, cfg.highlights, cfg.midtones, curve_lut, output);
        }
    }

    if (error) {
        fprintf(stderr, "Halide pipeline returned error %d\n", error);
        return 1;
    }

    // FIX: A single, unambiguous save call after all logic is complete.
    fprintf(stderr, "output: %s\n", cfg.output_path.c_str());
    save_processed_png(output, cfg.output_path);
    
    printf("Success!\n");
    return 0;
}
