#ifndef STAGE_APPLY_CURVE_H
#define STAGE_APPLY_CURVE_H

#include "Halide.h"

// This stage takes a uint16 input and applies the pre-computed uint16 LUT.
inline Halide::Func pipeline_apply_curve(Halide::Func input_u16,
                                         Halide::Func tone_curves_lut,
                                         Halide::Expr lut_extent,
                                         Halide::Expr curve_mode,
                                         Halide::Var x, Halide::Var y, Halide::Var c) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_APPLY_CURVE
    Func curved("curved_dummy");
    curved(x, y, c) = input_u16(x, y, c);
    return curved;
#else
    Func curved("curved");

    // --- Mode 1: RGB (per-channel) ---
    Func curved_rgb("curved_rgb");
    {
        // The input is already a clamped uint16, safe for LUT access.
        // We cast to int32 for the LUT lookup as it's a common index type.
        Expr val = cast<int32_t>(input_u16(x, y, c));
        curved_rgb(x, y, c) = tone_curves_lut(val, c);
    }

    // --- Mode 0: Luma ---
    Func curved_luma("curved_luma");
    {
        Expr r = cast<float>(input_u16(x, y, 0));
        Expr g = cast<float>(input_u16(x, y, 1));
        Expr b = cast<float>(input_u16(x, y, 2));

        Expr luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        Expr clamped_luma = clamp(cast<int32_t>(luma), 0, lut_extent - 1);
        
        Expr new_luma = cast<float>(tone_curves_lut(clamped_luma, 1)); // Green curve
        Expr luma_safe = select(luma > 1e-6f, luma, 1e-6f);
        
        Expr r_new = r * new_luma / luma_safe;
        Expr g_new = g * new_luma / luma_safe;
        Expr b_new = b * new_luma / luma_safe;
        
        curved_luma(x, y, c) = mux(c, {
            u16_sat(r_new), u16_sat(g_new), u16_sat(b_new)
        });
    }

    curved(x, y, c) = select(curve_mode == 0, curved_luma(x, y, c), curved_rgb(x, y, c));
    return curved;
#endif
}

#endif // STAGE_APPLY_CURVE_H
