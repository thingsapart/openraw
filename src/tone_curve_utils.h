#ifndef TONE_CURVE_UTILS_H
#define TONE_CURVE_UTILS_H

#include <vector>
#include <string>
#include <cstdint>

#include "HalideBuffer.h"
#include "process_options.h" // Include the new shared header


struct Point { float x, y; };

class ToneCurveUtils {
public:
    // Constructor
    ToneCurveUtils(const ProcessConfig& cfg);

    // Public method to get the generated LUT
    // The pipeline now operates on 16-bit data, so the LUT is also 16-bit.
    Halide::Runtime::Buffer<uint16_t, 2> get_lut_for_halide();

    // Public method to render the curve visualization
    bool render_curves_to_png(const char* filename, int width = 250, int height = 150);

private:
    // Parameters from config
    ProcessConfig config;
    int curve_mode;

    // LUT data is now 16-bit to match the pipeline's precision.
    Halide::Runtime::Buffer<uint16_t, 2> lut_buffer;
    
    // Private static helpers (declarations only)
    static bool parse_curve_points(const std::string& s, std::vector<Point>& points);
    static void generate_lut_channel(const ProcessConfig& cfg, const std::vector<Point>& user_points, uint16_t* lut_col, int lut_size);
};

#endif // TONE_CURVE_UTILS_H
