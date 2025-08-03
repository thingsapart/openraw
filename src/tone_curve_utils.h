#ifndef TONE_CURVE_UTILS_H
#define TONE_CURVE_UTILS_H

#include <vector>
#include <string>
#include <cstdint>

#include "HalideBuffer.h"

// All pipeline parameters are now encapsulated in this single struct.
struct ProcessConfig {
    std::string input_path;
    std::string output_path;
    float black_point = 25.0f;
    float white_point = 4095.0f;
    float exposure = 1.0f;
    int demosaic_algorithm = 1; // 0=simple, 1=vhg, 2=ahd, 3=lmmse
    float color_temp = 3700.0f;
    float tint = 0.0f;
    float saturation = 1.0f;
    int saturation_algorithm = 1; // 0=HSL, 1=LAB
    float gamma = 2.2f;
    float contrast = 50.0f;
    std::string curve_points_str;
    std::string curve_r_str;
    std::string curve_g_str;
    std::string curve_b_str;
    int curve_mode = 1; // 0=Luma, 1=RGB
    float sharpen = 1.0f;
    float ca_strength = 0.0f; // Default to off
    int timing_iterations = 5;
    int tonemap_algorithm = 3; // 0=linear, 1=reinhard, 2=filmic, 3=gamma
};


struct Point { float x, y; };

class ToneCurveUtils {
public:
    // Constructor
    ToneCurveUtils(const ProcessConfig& cfg);

    // Public method to get the generated LUT
    Halide::Runtime::Buffer<uint16_t, 2> get_lut_for_halide();

    // Public method to render the curve visualization
    bool render_curves_to_png(const char* filename, int width = 250, int height = 150);

private:
    // Parameters from config
    ProcessConfig config;
    int curve_mode;

    // LUT data
    Halide::Runtime::Buffer<uint16_t, 2> lut_buffer;
    
    // Private static helpers (declarations only)
    static bool parse_curve_points(const std::string& s, std::vector<Point>& points);
    static void generate_lut_channel(const std::vector<Point>& user_points, const ProcessConfig& cfg, uint16_t* lut_col, int lut_size);
};

#endif // TONE_CURVE_UTILS_H
