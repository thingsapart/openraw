#ifndef STAGE_VIGNETTE_H
#define STAGE_VIGNETTE_H

#include "Halide.h"
#include "pipeline_helpers.h"

class VignetteBuilder {
public:
    Halide::Func output;

    VignetteBuilder(Halide::Func input_srgb,
                    Halide::Expr out_width, Halide::Expr out_height,
                    Halide::Expr amount_slider, Halide::Expr midpoint_slider,
                    Halide::Expr roundness_slider, Halide::Expr highlights_slider,
                    Halide::Var x, Halide::Var y, Halide::Var c)
        : output("vignette_corrected")
    {
        using namespace Halide;

        // Normalize sliders from UI range [0-100] to calculation range [0-1].
        Expr amount = amount_slider * 0.01f;
        Expr midpoint = midpoint_slider * 0.01f;
        Expr roundness = roundness_slider * 0.01f;
        Expr highlight_protection = highlights_slider * 0.01f;

        // --- Shape (Roundness) ---
        Expr center_x = (cast<float>(out_width) - 1.0f) / 2.0f;
        Expr center_y = (cast<float>(out_height) - 1.0f) / 2.0f;
        Expr dx = cast<float>(x) - center_x;
        Expr dy = cast<float>(y) - center_y;

        Expr max_r = max(center_x, center_y);
        Expr min_r = min(center_x, center_y);

        // Lerp between a circular radius (based on min dimension) and an elliptical one (based on individual dimensions).
        Expr scale_x = lerp(min_r, center_x, roundness);
        Expr scale_y = lerp(min_r, center_y, roundness);

        Expr norm_x = dx / (scale_x + 1e-6f);
        Expr norm_y = dy / (scale_y + 1e-6f);
        Expr r_sq = norm_x * norm_x + norm_y * norm_y;

        // --- Falloff (Midpoint) ---
        // The midpoint slider controls the exponent of the falloff curve.
        // Its mapping is non-linear to provide more fine-grained control for
        // low-exponent (soft, broad) vignettes.
        Expr r = sqrt(max(0.0f, r_sq));
        Expr exponent = 0.25f * fast_pow(32.0f, midpoint); // Maps midpoint [0,1] to exponent [0.25, 8.0] non-linearly
        Expr polynomial = fast_pow(r, exponent);

        // --- Amount ---
        Expr vignette_factor = 1.0f - amount * polynomial;

        // --- Highlight Protection ---
        Expr luma = 0.299f * input_srgb(x, y, 0) + 0.587f * input_srgb(x, y, 1) + 0.114f * input_srgb(x, y, 2);
        // Blend starts at a luma of 0.75, fully active at 1.0.
        Expr highlight_blend = smoothstep(0.75f, 1.0f, luma);
        Expr protection_factor = lerp(vignette_factor, 1.0f, highlight_blend * highlight_protection);

        // Only apply highlight protection when brightening (amount > 0), not when darkening.
        Expr final_factor = select(amount > 0, protection_factor, vignette_factor);

        output(x, y, c) = input_srgb(x, y, c) * final_factor;
    }
};

#endif // STAGE_VIGNETTE_H

