#ifndef TONE_CURVE_UTILS_H
#define TONE_CURVE_UTILS_H

#include <vector>
#include <string>
#include <cstdint>

#include "HalideBuffer.h"
#include "process_options.h" // Include the new shared header which defines Point

/**
 * @namespace ToneCurveUtils
 * @brief A collection of host-side static utility functions for tone curve management.
 *
 * This namespace provides functions to translate high-level parameters from `ProcessConfig`
 * into a 1D lookup table (LUT) that the Halide pipeline can efficiently consume.
 * It ensures that the logic for generating curves is centralized and consistent
 * between the command-line runner and the interactive UI.
 */
namespace ToneCurveUtils {

    /**
     * @brief Generates the final, combined LUT for the Halide pipeline.
     * @param cfg The process configuration, containing tonemap settings and curve points.
     * @return A 65536x3 16-bit Halide buffer containing the complete tone curve.
     *
     * This function performs the full two-stage mapping:
     * 1. Applies the base tonemap operator (gamma, filmic, etc.).
     * 2. Applies the user's custom curve (or a default contrast curve) on top.
     * The result is the definitive LUT that the image processing pipeline will use.
     */
    Halide::Runtime::Buffer<uint16_t, 2> generate_pipeline_lut(const ProcessConfig& cfg);

    /**
     * @brief Generates a LUT that only reflects the user's curve against a linear baseline.
     * @param cfg The process configuration, containing the curve points.
     * @param out_lut The Halide buffer to be filled with the generated curve.
     *
     * This function performs only the second stage of the mapping (the user curve)
     * against a linear input. This is used by the UI's curve editor to provide a
     * direct, intuitive visualization of the curve being edited, without the
     * base tonemap applied. The core interpolation logic used here is identical
     * to the one used for the pipeline LUT, ensuring consistency.
     */
    void generate_linear_lut(const ProcessConfig& cfg, Halide::Runtime::Buffer<uint16_t, 2>& out_lut);
    
    // Renders the combined curve to a PNG file (for command-line use).
    bool render_curves_to_png(const ProcessConfig& cfg, const char* filename, int width = 250, int height = 150);

    // Parses a curve string like "0:0,0.5:0.6,1:1" into a vector of Points.
    bool parse_curve_points(const std::string& s, std::vector<Point>& points);
    
    // Converts a vector of Points back into a string representation.
    std::string points_to_string(const std::vector<Point>& points);

} // namespace ToneCurveUtils

#endif // TONE_CURVE_UTILS_H
