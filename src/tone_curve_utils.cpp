#include "tone_curve_utils.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <set>

// The STB implementation define should only exist in ONE .cpp file.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace ToneCurveUtils {

// --- Private Namespace Helpers ---
namespace {

// Helper for the filmic tonemapping curve calculation.
float uncharted2_tonemap_partial(float x) {
    const float A = 0.22f, B = 0.30f, C = 0.10f, D = 0.20f, E = 0.01f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// Implements Cubic Hermite spline interpolation using basis functions.
float hermite_interpolate(float p1, float p2, float t1, float tangent2, float h, float t) {
    float t_sq = t * t;
    float t3 = t_sq * t;
    float h00 = 2*t3 - 3*t_sq + 1;
    float h10 = t3 - 2*t_sq + t;
    float h01 = -2*t3 + 3*t_sq;
    float h11 = t3 - t_sq;
    return h00*p1 + h10*h*t1 + h01*p2 + h11*h*tangent2;
}

// Simple sign function
template <typename T> int sign(T val) {
    return (T(0) < val) - (val < T(0));
}

// The core LUT generation function, now implementing a Monotone Cubic Hermite Spline.
void generate_lut_channel(const ProcessConfig& cfg, const std::vector<Point>& user_points, uint16_t* lut_col, int lut_size, bool apply_base_tonemap) {
    std::vector<Point> points = user_points;
    if (points.empty()) {
        // If no user points, generate a default S-curve based on contrast and fall back.
        for (int i = 0; i < lut_size; ++i) {
            float linear_val = static_cast<float>(i) / (lut_size - 1.0f);
            float tonemapped_val = apply_base_tonemap ? powf(linear_val, 1.0f/2.2f) : linear_val;
            float b = 2.0f - powf(2.0f, cfg.contrast / 100.0f);
            float a = 2.0f - 2.0f * b;
            float final_val = (tonemapped_val > 0.5f) ?
                1.0f - (a * (1.0f - tonemapped_val) * (1.0f - tonemapped_val) + b * (1.0f - tonemapped_val)) :
                a * tonemapped_val * tonemapped_val + b * tonemapped_val;
            lut_col[i] = static_cast<uint16_t>(std::max(0.0f, std::min(65535.0f, final_val * 65535.0f + 0.5f)));
        }
        return;
    }
    
    if (points.front().x > 1e-6) points.insert(points.begin(), {0.0f, 0.0f});
    if (points.back().x < 1.0 - 1e-6) points.push_back({1.0f, 1.0f});

    if (points.size() < 2) { /* Invalid state, should not happen */ return; }

    // STEP 1: Calculate initial tangents (slopes) at each control point.
    std::vector<float> tangents(points.size());
    if (points.size() == 2) {
        float slope = (points[1].y - points[0].y) / (points[1].x - points[0].x);
        tangents[0] = slope;
        tangents[1] = slope;
    } else {
        // Endpoints use a one-sided difference for a natural, straight tangent.
        tangents[0] = (points[1].y - points[0].y) / (points[1].x - points[0].x);
        for (size_t i = 1; i < points.size() - 1; ++i) {
            tangents[i] = (points[i+1].y - points[i-1].y) / (points[i+1].x - points[i-1].x);
        }
        tangents[points.size()-1] = (points[points.size()-1].y - points[points.size()-2].y) /
                                    (points[points.size()-1].x - points[points.size()-2].x);
    }
    
    // STEP 2: Enforce monotonicity constraints on the tangents.
    for (size_t i = 0; i < points.size() - 1; ++i) {
        float dx = points[i+1].x - points[i].x;
        float dy = points[i+1].y - points[i].y;
        if (fabsf(dx) < 1e-6f) continue;
        float slope = dy / dx;

        if (fabsf(slope) < 1e-6f) {
            tangents[i] = 0;
            tangents[i+1] = 0;
        } else {
            if (sign(tangents[i]) != sign(slope)) tangents[i] = 0;
            if (sign(tangents[i+1]) != sign(slope)) tangents[i+1] = 0;

            float alpha = tangents[i] / slope;
            float beta = tangents[i+1] / slope;
            if (alpha*alpha + beta*beta > 9.0f) {
                float tau = 3.0f / sqrtf(alpha*alpha + beta*beta);
                tangents[i] = tau * alpha * slope;
                tangents[i+1] = tau * beta * slope;
            }
        }
    }

    // STEP 3: Generate the LUT by interpolating each segment.
    size_t k = 0;
    for (int i = 0; i < lut_size; ++i) {
        float linear_val = static_cast<float>(i) / (lut_size - 1.0f);
        float tonemapped_val = apply_base_tonemap ? powf(linear_val, 1.0f/2.2f) : linear_val;

        while (k < points.size() - 2 && tonemapped_val > points[k+1].x) {
            k++;
        }

        const Point& p1 = points[k];
        const Point& p2 = points[k+1];
        float h = p2.x - p1.x;
        float t = (h > 1e-6f) ? (tonemapped_val - p1.x) / h : 0.0f;
        
        float final_val = hermite_interpolate(p1.y, p2.y, tangents[k], tangents[k+1], h, t);

        lut_col[i] = static_cast<uint16_t>(std::max(0.0f, std::min(65535.0f, final_val * 65535.0f + 0.5f)));
    }
}

// Helper to evaluate a single curve (represented by points) at a specific x-coordinate.
float evaluate_curve_at(const std::vector<Point>& points, float x) {
    if (points.empty()) return x; // Return identity if no curve
    // Use a temporary LUT to evaluate the curve, ensuring the logic is identical to generation.
    uint16_t lut_val;
    ProcessConfig temp_cfg; // Use default config for evaluation
    generate_lut_channel(temp_cfg, points, &lut_val, 1, false);
    
    // Since we only generate one value, we need to find it in the full range.
    // This is a bit inefficient but reuses the complex spline logic perfectly.
    std::vector<uint16_t> lut_full(2);
    generate_lut_channel(temp_cfg, points, lut_full.data(), 2, false);
    
    std::vector<uint16_t> lut_single(1);
    int lut_index = static_cast<int>(x * 65535.0f);
    generate_lut_channel(temp_cfg, points, &lut_single[0], 1, false);

    std::vector<uint16_t> temp_lut(65536);
    generate_lut_channel(temp_cfg, points, temp_lut.data(), 65536, false);
    return (float)temp_lut[lut_index] / 65535.0f;
}

} // anonymous namespace


// --- Public Function Implementations ---

Halide::Runtime::Buffer<uint16_t, 2> generate_pipeline_lut(const ProcessConfig& cfg) {
    Halide::Runtime::Buffer<uint16_t, 2> lut_buffer(65536, 3);
    const auto& luma_pts = cfg.curve_points_luma;
    const auto& r_pts = cfg.curve_points_r;
    const auto& g_pts = cfg.curve_points_g;
    const auto& b_pts = cfg.curve_points_b;

    bool has_luma_curve = !luma_pts.empty();
    bool has_r_curve = !r_pts.empty();
    bool has_g_curve = !g_pts.empty();
    bool has_b_curve = !b_pts.empty();
    
    generate_lut_channel(cfg, has_r_curve ? r_pts : (has_luma_curve ? luma_pts : std::vector<Point>()), &lut_buffer(0, 0), lut_buffer.width(), true);
    generate_lut_channel(cfg, has_g_curve ? g_pts : (has_luma_curve ? luma_pts : std::vector<Point>()), &lut_buffer(0, 1), lut_buffer.width(), true);
    generate_lut_channel(cfg, has_b_curve ? b_pts : (has_luma_curve ? luma_pts : std::vector<Point>()), &lut_buffer(0, 2), lut_buffer.width(), true);
    
    return lut_buffer;
}

void generate_linear_lut(const ProcessConfig& cfg, Halide::Runtime::Buffer<uint16_t, 2>& out_lut) {
    const auto& luma_pts = cfg.curve_points_luma;
    const auto& r_pts = cfg.curve_points_r;
    const auto& g_pts = cfg.curve_points_g;
    const auto& b_pts = cfg.curve_points_b;

    bool has_luma_curve = !luma_pts.empty();
    bool has_r_curve = !r_pts.empty();
    bool has_g_curve = !g_pts.empty();
    bool has_b_curve = !b_pts.empty();
    
    generate_lut_channel(cfg, has_r_curve ? r_pts : (has_luma_curve ? luma_pts : std::vector<Point>()), &out_lut(0, 0), out_lut.width(), false);
    generate_lut_channel(cfg, has_g_curve ? g_pts : (has_luma_curve ? luma_pts : std::vector<Point>()), &out_lut(0, 1), out_lut.width(), false);
    generate_lut_channel(cfg, has_b_curve ? b_pts : (has_luma_curve ? luma_pts : std::vector<Point>()), &out_lut(0, 2), out_lut.width(), false);
}

void average_rgb_to_luma(ProcessConfig& cfg) {
    std::set<float> x_coords;
    for (const auto& p : cfg.curve_points_r) x_coords.insert(p.x);
    for (const auto& p : cfg.curve_points_g) x_coords.insert(p.x);
    for (const auto& p : cfg.curve_points_b) x_coords.insert(p.x);

    cfg.curve_points_luma.clear();
    for (float x : x_coords) {
        float y_r = evaluate_curve_at(cfg.curve_points_r, x);
        float y_g = evaluate_curve_at(cfg.curve_points_g, x);
        float y_b = evaluate_curve_at(cfg.curve_points_b, x);
        cfg.curve_points_luma.push_back({x, (y_r + y_g + y_b) / 3.0f});
    }
}

bool render_curves_to_png(const ProcessConfig& cfg, const char* filename, int width, int height) {
    auto lut_buffer = generate_pipeline_lut(cfg);
    // ... rest of function is unchanged ...
    return true;
}

bool parse_curve_points(const std::string& s, std::vector<Point>& points) {
    // ... unchanged ...
    return true;
}

std::string points_to_string(const std::vector<Point>& points) {
    // ... unchanged ...
    return "";
}

} // namespace ToneCurveUtils
