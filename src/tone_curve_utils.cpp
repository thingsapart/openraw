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
    generate_lut_channel(has_r_curve ? r_pts : (has_global_curve ? global_pts : std::vector<Point>()), config, &lut_buffer(0, 0), lut_buffer.width());
    generate_lut_channel(has_g_curve ? g_pts : (has_global_curve ? global_pts : std::vector<Point>()), config, &lut_buffer(0, 1), lut_buffer.width());
    generate_lut_channel(has_b_curve ? b_pts : (has_global_curve ? global_pts : std::vector<Point>()), config, &lut_buffer(0, 2), lut_buffer.width());
}

Halide::Runtime::Buffer<uint16_t, 2> ToneCurveUtils::get_lut_for_halide() {
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
        int prev_y = height - 1 - (int)((float)lut_buffer(0, channel_idx) / 65535.0f * (height-1));
        for(int x = 1; x < width; ++x) {
            int lut_idx = (int)(((float)x / (width-1)) * (lut_buffer.width()-1));
            int y = height - 1 - (int)((float)lut_buffer(lut_idx, channel_idx) / 65535.0f * (height-1));
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

void ToneCurveUtils::generate_lut_channel(const std::vector<Point>& user_points, const ProcessConfig& cfg, uint16_t* lut_col, int lut_size) {
    bool useCustomCurve = !user_points.empty();
    
    if (useCustomCurve) {
        // --- LOGIC for Custom Curves ---
        std::vector<Point> points = user_points;
        bool has_start = false, has_end = false;
        for(const auto& p : points) {
            if (p.x == 0.0f) has_start = true;
            if (p.x == 1.0f) has_end = true;
        }
        if (!has_start) points.insert(points.begin(), {0.0f, 0.0f});
        if (!has_end) points.push_back({1.0f, 1.0f});
        std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
        
        std::vector<float> tangents;
        if (points.size() > 1) {
            tangents.resize(points.size());
            if (points.size() == 2) {
                tangents[0] = tangents[1] = (points[1].y - points[0].y) / (points[1].x - points[0].x);
            } else {
                tangents[0] = (points[1].y - points[0].y) / (points[1].x - points[0].x);
                for (size_t i = 1; i < points.size() - 1; ++i) {
                    float slope_prev = (points[i].y - points[i-1].y) / (points[i].x - points[i-1].x);
                    float slope_next = (points[i+1].y - points[i].y) / (points[i+1].x - points[i].x);
                    tangents[i] = (slope_prev * slope_next <= 0) ? 0 : (slope_prev + slope_next) / 2.0f;
                }
                tangents.back() = (points.back().y - points[points.size()-2].y) / (points.back().x - points[points.size()-2].x);
            }
        }
        
        for (int i = 0; i < lut_size; ++i) {
            float val_norm = (float)i / (lut_size - 1.0f);
            float mapped_val;

            size_t k = 0;
            for (size_t j = 0; j < points.size() - 1; ++j) {
                if (val_norm >= points[j].x && val_norm <= points[j+1].x) {
                    k = j;
                    break;
                }
            }

            const Point& p0 = points[k];
            const Point& p1 = points[k+1];
            float m0 = tangents[k];
            float m1 = tangents[k+1];
            
            float h = p1.x - p0.x;
            if (h < 1e-6f) {
                mapped_val = p0.y;
            } else {
                float t = (val_norm - p0.x) / h;
                float t2 = t * t;
                float t3 = t2 * t;
                float h00 = 2*t3 - 3*t2 + 1;
                float h10 = t3 - 2*t2 + t;
                float h01 = -2*t3 + 3*t2;
                float h11 = t3 - t2;
                mapped_val = h00*p0.y + h10*h*m0 + h01*p1.y + h11*h*m1;
            }
            lut_col[i] = static_cast<uint16_t>(std::max(0.0f, std::min(65535.0f, mapped_val * 65535.0f + 0.5f)));
        }
    } else {
        // --- REVISED LOGIC for Gamma/Contrast S-Curve ---
        float black_level = cfg.black_point * cfg.exposure;
        // The white point should also be scaled by exposure, as it's a linear operation
        // before the curve. This was the source of the "too dark" issue, as it
        // was hardcoded to 65535.0f, ignoring the actual signal range.
        float white_level = cfg.white_point * cfg.exposure;
        if (white_level > 65535.0f) {
            white_level = 65535.0f;
        }

        if (black_level >= white_level) {
            black_level = white_level - 1;
        }
        float inv_range = 1.0f / (white_level - black_level);

        for (int i = 0; i < lut_size; ++i) {
            // 1. Normalize linear input to [0, 1]
            float linear_val = std::max(0.0f, std::min(1.0f, (i - black_level) * inv_range));
            
            // 2. Apply standard gamma correction to move to a perceptual space
            float perceptual_val = pow(linear_val, 1.0f / cfg.gamma);

            // 3. Apply contrast S-curve to the perceptually-encoded value
            float b = 2.0f - pow(2.0f, cfg.contrast / 100.0f);
            float a = 2.0f - 2.0f * b;
            float mapped_val;
            if (perceptual_val > 0.5f) {
                mapped_val = 1.0f - (a * (1.0f - perceptual_val) * (1.0f - perceptual_val) + b * (1.0f - perceptual_val));
            } else {
                mapped_val = a * perceptual_val * perceptual_val + b * perceptual_val;
            }
            
            lut_col[i] = static_cast<uint16_t>(std::max(0.0f, std::min(65535.0f, mapped_val * 65535.0f + 0.5f)));
        }
    }
}
