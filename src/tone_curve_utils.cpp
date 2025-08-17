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
    if (user_points.empty()) {
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

    Spline s(user_points, 1.0f, false, true);

    for (int i = 0; i < lut_size; ++i) {
        float linear_val = static_cast<float>(i) / (lut_size - 1.0f);
        float tonemapped_val = apply_base_tonemap ? powf(linear_val, 1.0f/2.2f) : linear_val;
        float final_val = s.evaluate(tonemapped_val);
        lut_col[i] = static_cast<uint16_t>(std::max(0.0f, std::min(65535.0f, final_val * 65535.0f + 0.5f)));
    }
}

} // anonymous namespace


// --- Public Function Implementations ---

Spline::Spline(const std::vector<Point>& points, float default_y, bool is_additive, bool is_identity)
    : points_internal(points), is_empty(points.empty()), default_y_val(default_y),
      is_additive_val(is_additive), is_identity_val(is_identity) {
    if (is_empty) return;

    // Ensure spline is well-defined by adding endpoints if missing.
    // Different defaults for different curve types.
    if (points_internal.front().x > 1e-6) {
        float y0 = is_identity_val ? 0.0f : (is_additive_val ? 0.0f : 1.0f);
        if (!points_internal.empty()) y0 = points_internal.front().y;
        points_internal.insert(points_internal.begin(), {0.0f, y0});
    }
    if (points_internal.back().x < 1.0 - 1e-6) {
        float y1 = is_identity_val ? 1.0f : (is_additive_val ? 0.0f : 1.0f);
        if (points_internal.size() > 1) y1 = points_internal.back().y;
        points_internal.push_back({1.0f, y1});
    }

    if (points_internal.size() < 2) {
        is_empty = true;
        return;
    }

    tangents.resize(points_internal.size());
    if (points_internal.size() == 2) {
        float dx = points_internal[1].x - points_internal[0].x;
        float slope = (fabsf(dx) > 1e-6f) ? (points_internal[1].y - points_internal[0].y) / dx : 0.0f;
        tangents[0] = slope;
        tangents[1] = slope;
    } else {
        float dx0 = points_internal[1].x - points_internal[0].x;
        tangents[0] = (fabsf(dx0) > 1e-6f) ? (points_internal[1].y - points_internal[0].y) / dx0 : 0.0f;

        for (size_t i = 1; i < points_internal.size() - 1; ++i) {
            float dxi = points_internal[i+1].x - points_internal[i-1].x;
            tangents[i] = (fabsf(dxi) > 1e-6f) ? (points_internal[i+1].y - points_internal[i-1].y) / dxi : 0.0f;
        }
        size_t n = points_internal.size() - 1;
        float dxn = points_internal[n].x - points_internal[n-1].x;
        tangents[n] = (fabsf(dxn) > 1e-6f) ? (points_internal[n].y - points_internal[n-1].y) / dxn : 0.0f;
    }

    // Enforce monotonicity for tonal curves (is_identity), but not for HSL curves.
    if(is_identity_val) {
        for (size_t i = 0; i < points_internal.size() - 1; ++i) {
            float dx = points_internal[i+1].x - points_internal[i].x;
            if (fabsf(dx) < 1e-6f) continue;
            // This is the critical bug fix. It was comparing .y with .x.
            float slope = (points_internal[i+1].y - points_internal[i].y) / dx;
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
    }
}

float Spline::evaluate(float x) const {
    if (is_empty) {
        if (is_identity_val) return x;
        return is_additive_val ? 0.0f : default_y_val;
    }

    if (x <= points_internal.front().x) return points_internal.front().y;
    if (x >= points_internal.back().x) return points_internal.back().y;

    auto it = std::upper_bound(points_internal.begin(), points_internal.end(), x, [](float val, const Point& p){ return val < p.x; });
    size_t k = std::distance(points_internal.begin(), it) - 1;

    const Point& p1 = points_internal[k];
    const Point& p2 = points_internal[k+1];
    float h = p2.x - p1.x;
    float t = (h > 1e-6f) ? (x - p1.x) / h : 0.0f;

    return hermite_interpolate(p1.y, p2.y, tangents[k], tangents[k+1], h, t);
}


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
    
    // Ensure the curve is sampled at the endpoints
    x_coords.insert(0.0f);
    x_coords.insert(1.0f);

    Spline r_spline(cfg.curve_points_r, 1.0f, false, true);
    Spline g_spline(cfg.curve_points_g, 1.0f, false, true);
    Spline b_spline(cfg.curve_points_b, 1.0f, false, true);

    cfg.curve_points_luma.clear();
    for (float x : x_coords) {
        float y_r = r_spline.evaluate(x);
        float y_g = g_spline.evaluate(x);
        float y_b = b_spline.evaluate(x);
        cfg.curve_points_luma.push_back({x, (y_r + y_g + y_b) / 3.0f});
    }
}

bool render_curves_to_png(const ProcessConfig& cfg, const char* filename, int width, int height) {
    auto lut_buffer = generate_pipeline_lut(cfg);
    std::vector<uint8_t> pixels(width * height * 3);
    std::fill(pixels.begin(), pixels.end(), 20); // Dark background

    // Draw grid lines
    for (int i = 1; i < 4; ++i) {
        int x_pos = i * width / 4;
        int y_pos = i * height / 4;
        for (int y = 0; y < height; ++y) {
            for(int c=0; c<3; ++c) pixels[(y * width + x_pos) * 3 + c] = 50;
        }
        for (int x = 0; x < width; ++x) {
            for(int c=0; c<3; ++c) pixels[(y_pos * width + x) * 3 + c] = 50;
        }
    }

    // Draw curves
    for (int c = 0; c < 3; ++c) {
        for (int x = 0; x < width; ++x) {
            float norm_x = static_cast<float>(x) / (width - 1);
            int lut_idx = static_cast<int>(norm_x * (lut_buffer.width() - 1));
            float norm_y = static_cast<float>(lut_buffer(lut_idx, c)) / 65535.0f;
            int y = static_cast<int>((1.0f - norm_y) * (height - 1));
            if (y >= 0 && y < height) {
                for(int chan=0; chan<3; ++chan) {
                    if (chan == c) pixels[(y * width + x) * 3 + chan] = 255;
                    else pixels[(y * width + x) * 3 + chan] = 0;
                }
            }
        }
    }

    return stbi_write_png(filename, width, height, 3, pixels.data(), width * 3) != 0;
}

bool parse_curve_points(const std::string& s, std::vector<Point>& points) {
    points.clear();
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::stringstream item_ss(item);
        float x, y;
        char colon;
        if (item_ss >> x >> colon >> y && colon == ':') {
            points.push_back({x, y});
        } else {
            return false;
        }
    }
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b){ return a.x < b.x; });
    return true;
}

std::string points_to_string(const std::vector<Point>& points) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(3);
    for (size_t i = 0; i < points.size(); ++i) {
        ss << points[i].x << ":" << points[i].y;
        if (i < points.size() - 1) {
            ss << ",";
        }
    }
    return ss.str();
}

} // namespace ToneCurveUtils
