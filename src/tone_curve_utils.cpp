#include "tone_curve_utils.h"
#include <cmath>
#include <algorithm>
#include <iostream>

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

// --- IMPLEMENTATION ---

ToneCurveUtils::ToneCurveUtils(const ProcessConfig& cfg)
    : config(cfg), curve_mode(cfg.curve_mode), lut_buffer(65536, 3) {
    
    std::vector<Point> global_pts, r_pts, g_pts, b_pts;

    bool has_global_curve = parse_curve_points(config.curve_points_str, global_pts);
    bool has_r_curve = parse_curve_points(config.curve_r_str, r_pts);
    bool has_g_curve = parse_curve_points(config.curve_g_str, g_pts);
    bool has_b_curve = parse_curve_points(config.curve_b_str, b_pts);
    
    // Use the specific channel curve if provided, otherwise fall back to global, otherwise use gamma/contrast.
    generate_lut_channel(config, has_r_curve ? r_pts : (has_global_curve ? global_pts : std::vector<Point>()), &lut_buffer(0, 0), lut_buffer.width());
    generate_lut_channel(config, has_g_curve ? g_pts : (has_global_curve ? global_pts : std::vector<Point>()), &lut_buffer(0, 1), lut_buffer.width());
    generate_lut_channel(config, has_b_curve ? b_pts : (has_global_curve ? global_pts : std::vector<Point>()), &lut_buffer(0, 2), lut_buffer.width());
}

Halide::Runtime::Buffer<uint8_t, 2> ToneCurveUtils::get_lut_for_halide() {
    return lut_buffer;
}

bool ToneCurveUtils::render_curves_to_png(const char* filename, int width, int height) {
    std::vector<uint8_t> pixels(width * height * 3);
    auto set_pixel = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        pixels[(y * width + x) * 3 + 0] = r;
        pixels[(y * width + x) * 3 + 1] = g;
        pixels[(y * width + x) * 3 + 2] = b;
    };
    
    // 1. Fill background and draw grid
    for(int y = 0; y < height; ++y) {
        for(int x = 0; x < width; ++x) {
            bool is_grid = (x == width/4 || x == width/2 || x == 3*width/4 || y == height/4 || y == height/2 || y == 3*height/4);
            uint8_t bg = is_grid ? 60 : 40;
            set_pixel(x, y, bg, bg, bg);
        }
    }

    // 2. Draw diagonal reference line
    for(int x = 0; x < width; ++x) {
        int y = (int)(((float)x / (width-1)) * (height-1));
        set_pixel(x, height - 1 - y, 100, 100, 100);
    }

    // 3. Draw the curve(s)
    auto draw_curve = [&](int channel_idx, uint8_t r, uint8_t g, uint8_t b) {
        int prev_y = height - 1 - (int)((float)lut_buffer(0, channel_idx) / 255.0f * (height-1));
        for(int x = 1; x < width; ++x) {
            int lut_idx = (int)(((float)x / (width-1)) * (lut_buffer.width()-1));
            int y = height - 1 - (int)((float)lut_buffer(lut_idx, channel_idx) / 255.0f * (height-1));
            // Draw a line from (x-1, prev_y) to (x, y)
            int start_y = std::min(prev_y, y);
            int end_y = std::max(prev_y, y);
            for(int line_y = start_y; line_y <= end_y; ++line_y) {
                set_pixel(x, line_y, r, g, b);
            }
            set_pixel(x-1, prev_y, r, g, b);
            prev_y = y;
        }
    };
    
    if (curve_mode == 0) { // Luma
        draw_curve(1, 255, 255, 255); // Draw Green curve in white
    } else { // RGB
        draw_curve(0, 255, 80, 80);
        draw_curve(1, 80, 255, 80);
        draw_curve(2, 80, 80, 255);
    }
    
    return stbi_write_png(filename, width, height, 3, pixels.data(), width * 3) != 0;
}

// --- Static Helper Implementations ---

// Helper for the filmic tonemapping curve calculation.
static float uncharted2_tonemap_partial(float x) {
    const float A = 0.22f, B = 0.30f, C = 0.10f, D = 0.20f, E = 0.01f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

bool ToneCurveUtils::parse_curve_points(const std::string& s, std::vector<Point>& points) {
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

void ToneCurveUtils::generate_lut_channel(const ProcessConfig& cfg, const std::vector<Point>& user_points, uint8_t* lut_col, int lut_size) {
    for (int i = 0; i < lut_size; ++i) {
        // The LUT index 'i' corresponds to a linear input value from the pipeline.
        float linear_val = static_cast<float>(i) / (lut_size - 1.0f);

        // OPTIMIZATION: Apply the selected global tonemapping operator first.
        float tonemapped_val;
        switch(cfg.tonemap_algorithm) {
            case 0: // linear
                tonemapped_val = linear_val;
                break;
            case 1: // reinhard
            {
                float reinhard_val = linear_val / (linear_val + 1.0f);
                tonemapped_val = powf(reinhard_val, 1.0f / 1.5f);
                break;
            }
            case 2: // filmic
            {
                const float exposure_bias = 2.0f;
                const float W = 11.2f;
                float curve_val = uncharted2_tonemap_partial(linear_val * exposure_bias);
                float white_scale = 1.0f / uncharted2_tonemap_partial(W);
                tonemapped_val = curve_val * white_scale;
                break;
            }
            case 3: // gamma
            default:
            {
                tonemapped_val = powf(linear_val, 1.0f / 2.2f);
                break;
            }
        }
        
        // Now, apply the S-curve or custom curve to the already tonemapped value.
        float final_val;
        bool useCustomCurve = !user_points.empty();
        if (useCustomCurve) {
            std::vector<Point> points = user_points;
            if (points.front().x > 1e-6) points.insert(points.begin(), {0.0f, 0.0f});
            if (points.back().x < 1.0 - 1e-6) points.push_back({1.0f, 1.0f});

            size_t k = 0;
            for (size_t j = 0; j < points.size() - 1; ++j) {
                if (tonemapped_val >= points[j].x && tonemapped_val <= points[j+1].x) {
                    k = j;
                    break;
                }
            }

            const Point& p0 = points[k];
            const Point& p1 = points[k+1];
            float t = (p1.x - p0.x < 1e-6) ? 0.0f : (tonemapped_val - p0.x) / (p1.x - p0.x);
            final_val = p0.y * (1.0f - t) + p1.y * t; // Linear interpolation for now
        } else {
            // Default S-curve from the original pipeline
            float b = 2.0f - powf(2.0f, cfg.contrast / 100.0f);
            float a = 2.0f - 2.0f * b;
            if (tonemapped_val > 0.5f) {
                final_val = 1.0f - (a * (1.0f - tonemapped_val) * (1.0f - tonemapped_val) + b * (1.0f - tonemapped_val));
            } else {
                final_val = a * tonemapped_val * tonemapped_val + b * tonemapped_val;
            }
        }
        
        // Clamp and convert to the final 8-bit value for the LUT.
        lut_col[i] = static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, final_val * 255.0f + 0.5f)));
    }
}
