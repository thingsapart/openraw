#include "tone_curve_utils.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iomanip>

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

} // anonymous namespace


// --- Public Function Implementations ---

Halide::Runtime::Buffer<uint16_t, 2> generate_pipeline_lut(const ProcessConfig& cfg) {
    Halide::Runtime::Buffer<uint16_t, 2> lut_buffer(65536, 3);
    const auto& global_pts = cfg.curve_points_global;
    const auto& r_pts = cfg.curve_points_r;
    const auto& g_pts = cfg.curve_points_g;
    const auto& b_pts = cfg.curve_points_b;

    bool has_global_curve = !global_pts.empty();
    bool has_r_curve = !r_pts.empty();
    bool has_g_curve = !g_pts.empty();
    bool has_b_curve = !b_pts.empty();
    
    generate_lut_channel(cfg, has_r_curve ? r_pts : (has_global_curve ? global_pts : std::vector<Point>()), &lut_buffer(0, 0), lut_buffer.width(), true);
    generate_lut_channel(cfg, has_g_curve ? g_pts : (has_global_curve ? global_pts : std::vector<Point>()), &lut_buffer(0, 1), lut_buffer.width(), true);
    generate_lut_channel(cfg, has_b_curve ? b_pts : (has_global_curve ? global_pts : std::vector<Point>()), &lut_buffer(0, 2), lut_buffer.width(), true);
    
    return lut_buffer;
}

void generate_linear_lut(const ProcessConfig& cfg, Halide::Runtime::Buffer<uint16_t, 2>& out_lut) {
    const auto& pts = cfg.curve_points_global;
    bool has_curve = !pts.empty();
    generate_lut_channel(cfg, has_curve ? pts : std::vector<Point>(), &out_lut(0, 0), out_lut.width(), false);
    generate_lut_channel(cfg, has_curve ? pts : std::vector<Point>(), &out_lut(0, 1), out_lut.width(), false);
    generate_lut_channel(cfg, has_curve ? pts : std::vector<Point>(), &out_lut(0, 2), out_lut.width(), false);
}

bool render_curves_to_png(const ProcessConfig& cfg, const char* filename, int width, int height) {
    auto lut_buffer = generate_pipeline_lut(cfg);

    std::vector<uint8_t> pixels(width * height * 3);
    auto set_pixel = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        pixels[(y * width + x) * 3 + 0] = r;
        pixels[(y * width + x) * 3 + 1] = g;
        pixels[(y * width + x) * 3 + 2] = b;
    };
    
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            bool is_grid = (x == width/4 || x == width/2 || x == 3*width/4 || y == height/4 || y == height/2 || y == 3*height/4);
            uint8_t bg = is_grid ? 60 : 40;
            set_pixel(x, y, bg, bg, bg);
        }
    }

    for(int x = 0; x < width; ++x) {
        int y = (int)(((float)x / (width-1)) * (height-1));
        set_pixel(x, height - 1 - y, 100, 100, 100);
    }

    auto draw_curve = [&](int channel_idx, uint8_t r, uint8_t g, uint8_t b) {
        int prev_y = height - 1 - (int)((float)lut_buffer(0, channel_idx) / 65535.0f * (height-1));
        for(int x = 1; x < width; ++x) {
            int lut_idx = (int)(((float)x / (width-1)) * (lut_buffer.width()-1));
            int y = height - 1 - (int)((float)lut_buffer(lut_idx, channel_idx) / 65535.0f * (height-1));
            int start_y = std::min(prev_y, y);
            int end_y = std::max(prev_y, y);
            for(int line_y = start_y; line_y <= end_y; ++line_y) {
                set_pixel(x, line_y, r, g, b);
            }
            set_pixel(x-1, prev_y, r, g, b);
            prev_y = y;
        }
    };
    
    if (cfg.curve_mode == 0) {
        draw_curve(1, 255, 255, 255);
    } else {
        draw_curve(0, 255, 80, 80);
        draw_curve(1, 80, 255, 80);
        draw_curve(2, 80, 80, 255);
    }
    
    return stbi_write_png(filename, width, height, 3, pixels.data(), width * 3) != 0;
}

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
    size_t colon_pos = current_s.find(':');
    if (colon_pos == std::string::npos) return false;
    try {
        float x = std::stof(current_s.substr(0, colon_pos));
        float y = std::stof(current_s.substr(colon_pos + 1));
        points.push_back({x, y});
    } catch (...) { return false; }

    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
        return a.x < b.x;
    });
    return true;
}

std::string points_to_string(const std::vector<Point>& points) {
    if (points.empty()) {
        return "";
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4);
    for (size_t i = 0; i < points.size(); ++i) {
        ss << points[i].x << ":" << points[i].y;
        if (i < points.size() - 1) {
            ss << ",";
        }
    }
    return ss.str();
}

} // namespace ToneCurveUtils
