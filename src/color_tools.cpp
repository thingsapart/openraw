#include "color_tools.h"
#include "tone_curve_utils.h"
#include "pipeline_helpers.h"
#include <cmath>
#include <algorithm> // for std::min/max

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// --- Host-Side Implementation ---
namespace HostColor {
    using Halide::Runtime::Buffer;

    namespace {
        // C++ version of the smoothstep function for host-side code.
        float smoothstep(float edge0, float edge1, float x) {
            // Scale, bias and saturate x to 0..1 range
            float t = (x - edge0) / (edge1 - edge0);
            t = std::max(0.0f, std::min(1.0f, t));
            // Evaluate polynomial
            return t * t * (3.0f - 2.0f * t);
        }

        // Host-side versions of the L*a*b* conversion functions.
        // These must exactly match the logic in the Halide helpers.
        float lab_f_inv(float t) {
            const float delta = 6.0f / 29.0f;
            if (t > delta) {
                return t*t*t;
            } else {
                return 3.0f*delta*delta*(t - 16.0f/116.0f);
            }
        }
    }

    RGB lch_to_linear_srgb(float L, float C, float h_rads) {
        // Lch -> Lab
        float a = C * cosf(h_rads);
        float b = C * sinf(h_rads);

        // Lab -> XYZ
        const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
        float fY = (L + 16.0f) / 116.0f;
        float fX = a / 500.0f + fY;
        float fZ = fY - b / 200.0f;

        float X = lab_f_inv(fX) * Xn;
        float Y = lab_f_inv(fY) * Yn;
        float Z = lab_f_inv(fZ) * Zn;

        // XYZ -> linear sRGB
        float r =  3.2404542f*X - 1.5371385f*Y - 0.4985314f*Z;
        float g = -0.9692660f*X + 1.8760108f*Y + 0.0415560f*Z;
        float b_srgb =  0.0556434f*X - 0.2040259f*Y + 1.0572252f*Z;

        return {r, g, b_srgb};
    }

    Halide::Runtime::Buffer<float, 4> generate_color_lut(const ProcessConfig& cfg, int size) {
        Buffer<float, 4> lut(size, size, size, 3);

        ToneCurveUtils::Spline H_v_H(cfg.curve_hue_vs_hue, 0.0f, true);
        ToneCurveUtils::Spline H_v_S(cfg.curve_hue_vs_sat, 1.0f);
        ToneCurveUtils::Spline H_v_L(cfg.curve_hue_vs_lum, 0.0f, true);
        ToneCurveUtils::Spline L_v_S(cfg.curve_lum_vs_sat, 1.0f);
        ToneCurveUtils::Spline S_v_S(cfg.curve_sat_vs_sat, 1.0f, false, true); // Identity curve as default

        // The loop order MUST match the memory layout of the LUT for correctness.
        // Halide buffer layout is (dim0, dim1, dim2, ...), which in this case we
        // have defined as (L, C, H). The innermost loop must correspond to dim0.
        for (int h_i = 0; h_i < size; ++h_i) {
            for (int c_i = 0; c_i < size; ++c_i) {
                for (int l_i = 0; l_i < size; ++l_i) {
                    // 1. Denormalize grid coordinates to Lch values
                    float L_in = (float)l_i / (size - 1); // Normalized L [0, 1] for curves
                    float C_in_norm = (float)c_i / (size - 1); // Normalized C [0, 1] for curves
                    float h_in_norm = (float)h_i / (size - 1); // Normalized h [0, 1] for curves

                    float L_phys = L_in * 100.0f;
                    float C_phys = C_in_norm * 150.0f;
                    float h_rads = (h_in_norm * 2.f * M_PI) - M_PI; // Map to [-pi, pi]

                    float L_out = L_phys;
                    float C_out = C_phys;
                    float h_out_rads = h_rads;

                    // 2. Apply curves
                    h_out_rads += H_v_H.evaluate(h_in_norm) * M_PI; // Additive, scaled to +/- 180 deg
                    C_out *= H_v_S.evaluate(h_in_norm);
                    L_out += H_v_L.evaluate(h_in_norm) * 100.0f; // Additive, scaled to +/- 100 L
                    C_out *= L_v_S.evaluate(L_in);

                    // The Sat vs Sat curve maps a normalized saturation to a new normalized saturation.
                    // We must normalize the current C_out, evaluate, and then scale back.
                    C_out = S_v_S.evaluate(C_out / 150.0f) * 150.0f;

                    // 3. Apply color wheels (in Lab space)
                    float a = C_out * cosf(h_out_rads);
                    float b = C_out * sinf(h_out_rads);

                    float luma_norm = L_out / 100.f;
                    float shadow_w = 1.0f - smoothstep(0.0f, 0.5f, luma_norm);
                    float hi_w = smoothstep(0.5f, 1.0f, luma_norm);
                    float mid_w = 1.0f - shadow_w - hi_w;

                    const float wheel_scale = 50.0f; // Controls sensitivity of color wheels
                    a += (cfg.shadows_wheel.x * shadow_w + cfg.midtones_wheel.x * mid_w + cfg.highlights_wheel.x * hi_w) * wheel_scale;
                    b += (cfg.shadows_wheel.y * shadow_w + cfg.midtones_wheel.y * mid_w + cfg.highlights_wheel.y * hi_w) * wheel_scale;

                    L_out *= (1.0f + cfg.shadows_luma/100.f * shadow_w);
                    L_out *= (1.0f + cfg.midtones_luma/100.f * mid_w);
                    L_out *= (1.0f + cfg.highlights_luma/100.f * hi_w);

                    // 4. Convert back to Lch and store in LUT
                    C_out = sqrtf(a*a + b*b);
                    // Stabilize hue calculation for near-achromatic colors.
                    h_out_rads = (C_out > 1e-5f) ? atan2f(b, a) : 0.0f;

                    lut(l_i, c_i, h_i, 0) = L_out;
                    lut(l_i, c_i, h_i, 1) = C_out;
                    lut(l_i, c_i, h_i, 2) = h_out_rads;
                }
            }
        }
        return lut;
    }
}

