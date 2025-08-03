#ifndef STAGE_SATURATION_H
#define STAGE_SATURATION_H

#include "Halide.h"

// This stage contains two different saturation algorithms, selectable at runtime.
// 0: HSL-based saturation. Fast but less perceptually accurate.
// 1: L*a*b*-based chroma boosting. Slower but more perceptually uniform.
inline Halide::Func pipeline_saturation(Halide::Func input,
                                        Halide::Expr saturation,
                                        Halide::Expr algorithm,
                                        Halide::Var x, Halide::Var y, Halide::Var c) {
#ifdef NO_SATURATION
    Halide::Func saturated("saturated_dummy");
    saturated(x, y, c) = input(x, y, c);
    return saturated;
#else
    using namespace Halide;
    using namespace Halide::ConciseCasts;

    Func saturated("saturated");
    Func hsl_saturated("hsl_saturated");
    Func lab_saturated("lab_saturated");

    // CRITICAL FIX: The color science algorithms (HSL, L*a*b*) assume a [0, 1] input range.
    // The linear pipeline may contain highlight values > 1.0. We must clamp the input
    // to this stage to prevent mathematical errors that cause extreme color shifts.
    Func clamped_input("clamped_for_saturation");
    clamped_input(x, y, c) = clamp(input(x, y, c), 0.0f, 1.0f);

    Expr r_norm = clamped_input(x, y, 0);
    Expr g_norm = clamped_input(x, y, 1);
    Expr b_norm = clamped_input(x, y, 2);

    // Helper lambda for fmod(a, b) -> a - b * floor(a / b)
    auto halide_fmod = [](Halide::Expr a, Halide::Expr b) {
        return a - b * Halide::floor(a / b);
    };

    // --- Algorithm 0: HSL-based Saturation ---
    {
        Expr c_max = max(r_norm, g_norm, b_norm);
        Expr c_min = min(r_norm, g_norm, b_norm);
        Expr delta = c_max - c_min;

        Expr l = (c_max + c_min) / 2.0f;
        Expr s = select(delta < 1e-6f, 0.0f, delta / (1.0f - abs(2.0f * l - 1.0f)));
        
        Expr h_intermediate = select(c_max == r_norm, halide_fmod((g_norm - b_norm) / delta, 6.0f),
                                select(c_max == g_norm, (b_norm - r_norm) / delta + 2.0f,
                                                     (r_norm - g_norm) / delta + 4.0f));
        Expr h_before_wrap = select(delta < 1e-6f, 0.0f, 60.0f * h_intermediate);
        Expr h = select(h_before_wrap < 0.0f, h_before_wrap + 360.0f, h_before_wrap);

        Expr new_s = clamp(s * saturation, 0.0f, 1.0f);

        Expr c_hsl = (1.0f - abs(2.0f * l - 1.0f)) * new_s;
        Expr x_hsl = c_hsl * (1.0f - abs(halide_fmod(h / 60.0f, 2.0f) - 1.0f));
        Expr m_hsl = l - c_hsl / 2.0f;
        
        Expr r_hsl = select(h < 60.0f, c_hsl, select(h < 120.0f, x_hsl, select(h < 180.0f, 0.0f, select(h < 240.0f, 0.0f, select(h < 300.0f, x_hsl, c_hsl)))));
        Expr g_hsl = select(h < 60.0f, x_hsl, select(h < 120.0f, c_hsl, select(h < 180.0f, c_hsl, select(h < 240.0f, x_hsl, select(h < 300.0f, 0.0f, 0.0f)))));
        Expr b_hsl = select(h < 60.0f, 0.0f, select(h < 120.0f, 0.0f, select(h < 180.0f, x_hsl, select(h < 240.0f, c_hsl, select(h < 300.0f, c_hsl, x_hsl)))));

        Expr r_new = (r_hsl + m_hsl);
        Expr g_new = (g_hsl + m_hsl);
        Expr b_new = (b_hsl + m_hsl);

        hsl_saturated(x, y, c) = mux(c, {r_new, g_new, b_new});
    }

    // --- Algorithm 1: L*a*b*-based Saturation ---
    {
        // sRGB to XYZ conversion matrix
        Expr x_xyz = 0.4124f * r_norm + 0.3576f * g_norm + 0.1805f * b_norm;
        Expr y_xyz = 0.2126f * r_norm + 0.7152f * g_norm + 0.0722f * b_norm;
        Expr z_xyz = 0.0193f * r_norm + 0.1192f * g_norm + 0.9505f * b_norm;

        // XYZ to L*a*b* conversion (D65 whitepoint)
        const float xn = 0.95047f, yn = 1.0f, zn = 1.08883f;
        const float delta_val = 6.0f / 29.0f;
        auto f = [&](Expr t) {
            // OPTIMIZATION: Replace pow() with fast_pow()
            return select(t > delta_val * delta_val * delta_val, fast_pow(t, 1.0f / 3.0f), t / (3.0f * delta_val * delta_val) + 4.0f / 29.0f);
        };
        Expr fx = f(x_xyz / xn);
        Expr fy = f(y_xyz / yn);
        Expr fz = f(z_xyz / zn);

        Expr L = 116.0f * fy - 16.0f;
        Expr a = 500.0f * (fx - fy);
        Expr b = 200.0f * (fy - fz);

        Expr new_a = a * saturation;
        Expr new_b = b * saturation;

        auto f_inv = [&](Expr t) {
            return select(t > delta_val, t * t * t, 3.0f * delta_val * delta_val * (t - 4.0f / 29.0f));
        };
        Expr fy_new = (L + 16.0f) / 116.0f;
        Expr fx_new = new_a / 500.0f + fy_new;
        Expr fz_new = fy_new - new_b / 200.0f;
        
        Expr x_new_xyz = f_inv(fx_new) * xn;
        Expr y_new_xyz = f_inv(fy_new) * yn;
        Expr z_new_xyz = f_inv(fz_new) * zn;
        
        Expr r_new_norm =  3.2406f * x_new_xyz - 1.5372f * y_new_xyz - 0.4986f * z_new_xyz;
        Expr g_new_norm = -0.9689f * x_new_xyz + 1.8758f * y_new_xyz + 0.0415f * z_new_xyz;
        Expr b_new_norm =  0.0557f * x_new_xyz - 0.2040f * y_new_xyz + 1.0570f * z_new_xyz;

        lab_saturated(x, y, c) = mux(c, {r_new_norm, g_new_norm, b_new_norm});
    }

    // --- Final Selection ---
    // If saturation is 1.0 (a no-op), we can bypass the expensive color math
    // and just pass the original (unclamped) input through. This preserves highlight data.
    saturated(x, y, c) = select(abs(saturation - 1.0f) < 1e-4f, 
                                input(x, y, c),
                                select(algorithm == 0,
                                    hsl_saturated(x, y, c),
                                    lab_saturated(x, y, c))
                                );

    return saturated;
#endif
}

#endif // STAGE_SATURATION_H
