#ifndef STAGE_LOCAL_TONAL_ADJUSTMENTS_H
#define STAGE_LOCAL_TONAL_ADJUSTMENTS_H

#include "Halide.h"
#include "HalideBuffer.h"
#include "pipeline_helpers.h"
#include "halide_guided_filter.h"
#include <memory>

class LocalTonalAdjustmentsBuilder {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;

    // We now hold the builder by a pointer to expose its internals to the generator for scheduling.
    std::unique_ptr<Halide::GuidedFilterBuilder> gf_builder;
    Halide::Func tonal_base;

    LocalTonalAdjustmentsBuilder(Halide::Func input_float,
                                 Halide::Var x, Halide::Var y, Halide::Var c,
                                 Halide::Expr clarity,
                                 Halide::Expr shadows,
                                 Halide::Expr highlights,
                                 Halide::Expr blacks,
                                 Halide::Expr whites,
                                 Halide::Expr dehaze,
                                 Halide::Expr width, Halide::Expr height) {
        
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        // --- Constants and Slider Remapping ---
        Expr clarity_factor = 1.0f + clarity / 100.0f;
        Expr shadows_factor = shadows / 100.0f;     // -> [-1, 1]
        Expr highlights_factor = highlights / 100.0f; // -> [-1, 1]
        Expr dehaze_factor = dehaze / 100.0f;       // -> [0, 1]
        Expr blacks_level = blacks / 100.0f * 0.2f;   // -> [-0.2, 0.2]
        Expr whites_level = 1.0f + whites / 100.0f * 0.2f; // -> [0.8, 1.2]

        // --- 1. Base/Detail Decomposition using Guided Filter ---
        
        // STABILITY FIX: Clamp the input to the guided filter to prevent
        // the coefficient calculations from exploding with large highlight values.
        Func clamped_input("tonal_clamped_input");
        clamped_input(x, y, c) = clamp(input_float(x, y, c), 0.0f, 1.5f);
        intermediates.push_back(clamped_input);

        // The guide image is the luminance of the STABLE, clamped input.
        Func luma("tonal_luma");
        luma(x, y) = 0.299f * clamped_input(x, y, 0) + 0.587f * clamped_input(x, y, 1) + 0.114f * clamped_input(x, y, 2);
        intermediates.push_back(luma);
        
        // Parameters for the guided filter. A larger epsilon adds stability.
        int radius = 32;
        float eps = 1e-2f;
        
        // Instantiate the Guided Filter builder, passing the caller's Vars.
        gf_builder = std::make_unique<GuidedFilterBuilder>(clamped_input, luma, x, y, c, radius, eps);
        tonal_base = gf_builder->output; // Assign the output for easy access.
        
        // PRESERVE HIGHLIGHTS: The detail layer is the difference between the
        // ORIGINAL, UNCLAMPED input and the stable base layer. This moves
        // all highlight information (>1.5) into the detail layer.
        Func detail("tonal_detail");
        detail(x, y, c) = input_float(x, y, c) - tonal_base(x, y, c);
        intermediates.push_back(detail);

        // --- 2. Adjust Detail Layer (Clarity & Dehaze Contrast) ---
        Func adjusted_detail("adjusted_detail");
        Expr base_luma = 0.299f * tonal_base(x, y, 0) + 0.587f * tonal_base(x, y, 1) + 0.114f * tonal_base(x, y, 2);
        Expr dehaze_contrast_gain = 1.0f + dehaze_factor * base_luma * 2.0f;
        adjusted_detail(x, y, c) = detail(x, y, c) * clarity_factor * dehaze_contrast_gain;
        intermediates.push_back(adjusted_detail);
        
        // --- 3. Adjust Base Layer (Shadows, Highlights, Dehaze Airlight) ---
        Func adjusted_base("adjusted_base");
        
        Expr shadow_mask = 1.0f - smoothstep(0.0f, 0.5f, base_luma);
        Expr highlight_mask = smoothstep(0.5f, 1.0f, base_luma);

        Expr shadow_gain = pow(2.0f, -shadows_factor);
        Expr highlight_gain = pow(2.0f, highlights_factor);

        Expr tonal_gain = lerp(1.0f, shadow_gain, shadow_mask);
        tonal_gain = lerp(tonal_gain, highlight_gain, highlight_mask);
        
        Expr dehaze_darken_factor = 1.0f - dehaze_factor * 0.8f;

        // ARTIFACT FIX: Prevent pow() from receiving a negative base due to filter
        // ringing by clamping the base to be non-negative. This avoids NaNs.
        adjusted_base(x, y, c) = pow(max(tonal_base(x, y, c), 0.0f), tonal_gain) * dehaze_darken_factor;
        intermediates.push_back(adjusted_base);

        // --- 4. Recombine and Apply Blacks/Whites ---
        Func recombined("recombined");
        recombined(x, y, c) = adjusted_base(x, y, c) + adjusted_detail(x, y, c);
        
        Func final_tonal_adjust("final_tonal_adjust");
        Expr remap_denom = whites_level - blacks_level;
        Expr remap_denom_safe = select(remap_denom > 1e-4f, remap_denom, 1e-4f);
        final_tonal_adjust(x, y, c) = (recombined(x, y, c) - blacks_level) / remap_denom_safe;
        
        // --- 5. Final Output ---
        Expr is_default = clarity == 0.0f && shadows == 0.0f && highlights == 0.0f && 
                          blacks == 0.0f && whites == 0.0f && dehaze == 0.0f;
                          
        output = Func("local_tonal_adjusted");
        output(x, y, c) = select(is_default, input_float(x, y, c), final_tonal_adjust(x, y, c));
    }
};

#endif // STAGE_LOCAL_TONAL_ADJUSTMENTS_H
