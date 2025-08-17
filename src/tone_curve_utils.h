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
 */
namespace ToneCurveUtils {

    /**
     * @class Spline
     * @brief A monotone cubic Hermite spline for curve evaluation.
     */
    class Spline {
    public:
        /**
         * @brief Constructs a spline from a set of control points.
         * @param points The user-defined control points.
         * @param default_y The value to return for an empty curve.
         * @param is_additive If true, the default curve is y=0. If false, it's y=1.
         * @param is_identity If true, the default curve is y=x.
         */
        Spline(const std::vector<Point>& points, float default_y = 1.0f, bool is_additive = false, bool is_identity = false);

        /**
         * @brief Evaluates the spline at a given x-coordinate.
         * @param x The input value, expected to be in the [0, 1] range.
         * @return The evaluated y-value from the spline.
         */
        float evaluate(float x) const;

    private:
        std::vector<Point> points_internal;
        std::vector<float> tangents;
        bool is_empty;
        float default_y_val;
        bool is_additive_val;
        bool is_identity_val;
    };

    // Generates the final, combined LUT for the Halide pipeline.
    Halide::Runtime::Buffer<uint16_t, 2> generate_pipeline_lut(const ProcessConfig& cfg);

    // Generates a LUT that only reflects the user's curve against a linear baseline.
    void generate_linear_lut(const ProcessConfig& cfg, Halide::Runtime::Buffer<uint16_t, 2>& out_lut);

    // Creates a new Luma curve by averaging the R, G, and B curves.
    void average_rgb_to_luma(ProcessConfig& cfg);

    // Renders the combined curve to a PNG file (for command-line use).
    bool render_curves_to_png(const ProcessConfig& cfg, const char* filename, int width = 250, int height = 150);

    // Parses a curve string like "0:0,0.5:0.6,1:1" into a vector of Points.
    bool parse_curve_points(const std::string& s, std::vector<Point>& points);

    // Converts a vector of Points back into a string representation.
    std::string points_to_string(const std::vector<Point>& points);

} // namespace ToneCurveUtils

#endif // TONE_CURVE_UTILS_H
