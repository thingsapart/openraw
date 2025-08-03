#ifndef STAGE_APPLY_CURVE_H
#define STAGE_APPLY_CURVE_H

#include "Halide.h"

// This stage takes a uint16 input and applies the pre-computed uint8 LUT
// to produce the final 8-bit output.
inline Halide::Func pipeline_apply_curve(Halide::Func input_u16,
                                         Halide::Func tone_curves_lut,
                                         Halide::Expr lut_extent,
                                         Halide::Expr curve_mode,
                                         Halide::Var x, Halide::Var y, Halide::Var c) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_APPLY_CURVE
    Func curved("curved_dummy");
    // Dummy pass-through stage, now converts to u8
    curved(x, y, c) = u8_sat(input_u16(x, y, c) >> 8);
    return curved;
#else
    Func curved("curved");

    // --- Mode 1: RGB (per-channel) ---
    // This path is already correct. It looks up the uint8 LUT and produces a uint8.
    Func curved_rgb("curved_rgb");
    {
        Expr val = cast<int32_t>(input_u16(x, y, c));
        curved_rgb(x, y, c) = tone_curves_lut(val, c);
    }

    // --- Mode 0: Luma (Color Preserving) ---
    // This path is now fixed to produce a uint8 result.
    Func curved_luma("curved_luma");
    {
        // 1. Normalize the 16-bit input to a [0, 1] float range.
        Expr r_f = cast<float>(input_u16(x, y, 0)) / 65535.f;
        Expr g_f = cast<float>(input_u16(x, y, 1)) / 65535.f;
        Expr b_f = cast<float>(input_u16(x, y, 2)) / 65535.f;

        // 2. Calculate the original luma of the float pixel.
        Expr luma_f = 0.2126f * r_f + 0.7152f * g_f + 0.0722f * b_f;
        
        // 3. Use the original luma to look up the new 8-bit luma from the LUT.
        // The LUT index must be scaled from the [0, 1] float range to the [0, 65535] integer range.
        Expr lut_idx = clamp(cast<int32_t>(luma_f * (lut_extent - 1)), 0, lut_extent - 1);
        Expr new_luma_u8 = tone_curves_lut(lut_idx, 1); // Use the Green curve for luma.
        
        // 4. Normalize the new 8-bit luma to a [0, 1] float.
        Expr new_luma_f = cast<float>(new_luma_u8) / 255.f;

        // 5. Calculate the scaling ratio and apply it to the original float RGB values to preserve hue.
        Expr luma_f_safe = select(luma_f > 1e-6f, luma_f, 1e-6f);
        Expr ratio = new_luma_f / luma_f_safe;
        
        Expr r_new_f = r_f * ratio;
        Expr g_new_f = g_f * ratio;
        Expr b_new_f = b_f * ratio;
        
        // 6. Convert the final float RGB values to uint8, which is the final output type.
        curved_luma(x, y, c) = mux(c, {
            u8_sat(r_new_f * 255.f), 
            u8_sat(g_new_f * 255.f), 
            u8_sat(b_new_f * 255.f)
        });
    }

    // Now both branches have a matching uint8 type.
    curved(x, y, c) = select(curve_mode == 0, curved_luma(x, y, c), curved_rgb(x, y, c));
    return curved;
#endif
}

#endif // STAGE_APPLY_CURVE_H
