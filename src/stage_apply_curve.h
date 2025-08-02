#ifndef STAGE_APPLY_CURVE_H
#define STAGE_APPLY_CURVE_H

#include "Halide.h"

// This stage now takes a pre-computed 2D Look-Up Table (LUT) of size [lut_size, 3]
// and applies it to the image.
// **FIX:** It now performs a 16-bit -> 16-bit transform.
inline Halide::Func pipeline_apply_curve(Halide::Func input,
                                         Halide::Func tone_curves_lut,
                                         Halide::Expr lut_extent,
                                         Halide::Expr curve_mode,
                                         Halide::Var x, Halide::Var y, Halide::Var c) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_APPLY_CURVE
    Func curved("curved_dummy");
    curved(x, y, c) = input(x,y,c);
    return curved;
#else
    Func curved("curved");

    // --- Mode 1: RGB (per-channel) ---
    // This is the simplest mode. We look up each channel in its corresponding LUT column.
    Func curved_rgb("curved_rgb");
    {
        // Cast the uint16 input to int32 to match the type of lut_extent.
        Expr val_as_int32 = cast<int32_t>(input(x, y, c));
        Expr clamped_val = clamp(val_as_int32, 0, lut_extent - 1);
        curved_rgb(x, y, c) = tone_curves_lut(clamped_val, c);
    }

    // --- Mode 0: Luma ---
    // Apply the Green channel's curve (column 1) to luminance to preserve hue.
    Func curved_luma("curved_luma");
    {
        Expr r = cast<float>(input(x, y, 0));
        Expr g = cast<float>(input(x, y, 1));
        Expr b = cast<float>(input(x, y, 2));

        // Calculate luminance using sRGB coefficients
        Expr luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        Expr clamped_luma = clamp(cast<int32_t>(luma), 0, lut_extent - 1);
        
        // Get the new luma from the Green channel's curve (index 1)
        Expr new_luma = cast<float>(tone_curves_lut(clamped_luma, 1));

        // Avoid division by zero for grayscale pixels
        Expr luma_safe = select(luma > 1e-6f, luma, 1e-6f);
        
        // Re-apply the original color ratio to the new luma
        Expr r_new = r * new_luma / luma_safe;
        Expr g_new = g * new_luma / luma_safe;
        Expr b_new = b * new_luma / luma_safe;
        
        curved_luma(x, y, c) = mux(c, {
            cast<uint16_t>(clamp(r_new, 0, 65535)),
            cast<uint16_t>(clamp(g_new, 0, 65535)),
            cast<uint16_t>(clamp(b_new, 0, 65535))
        });
    }

    // Select the final algorithm based on the curve_mode parameter.
    curved(x, y, c) = cast<uint16_t>(select(curve_mode == 0, curved_luma(x, y, c), curved_rgb(x, y, c)));

    return curved;
#endif
}

#endif // STAGE_APPLY_CURVE_H
