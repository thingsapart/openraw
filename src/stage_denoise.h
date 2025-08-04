#ifndef STAGE_DENOISE_H
#define STAGE_DENOISE_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include "halide_guided_filter.h"
#include <vector>
#include <type_traits>
#include <memory>

namespace { // Anonymous namespace for helpers

/**
 * A 2D-only version of the GuidedFilterBuilder.
 * This is a copy of the original 3D version, modified to work on
 * single-channel (2D) Funcs, as required for raw denoising.
 */
class GuidedFilter2DBuilder {
public:
    Halide::Func output;

    GuidedFilter2DBuilder(Halide::Func image, Halide::Func guide, Halide::Var x, Halide::Var y, int radius, Halide::Expr eps) {
        using namespace Halide;
        
        std::string prefix = "gf2d_r" + std::to_string(radius);

        int s = 1; // No subsampling for denoising for max quality
        int R = radius;

        Func small_guide(prefix + "_small_guide"), small_image(prefix + "_small_image");
        small_guide(x, y) = guide(x * s, y * s);
        small_image(x, y) = image(x * s, y * s);

        Func box_I(prefix + "_box_I"), box_II(prefix + "_box_II");
        box_I(x, y) = small_guide(x, y);
        box_II(x, y) = small_guide(x, y) * small_guide(x, y);

        Func box_p(prefix + "_box_p"), box_Ip(prefix + "_box_Ip");
        box_p(x, y) = small_image(x, y);
        box_Ip(x, y) = small_guide(x, y) * small_image(x, y);

        Func mean_I = box_filter_2d(box_I, R, R, prefix + "_mean_I", x, y);
        Func mean_II = box_filter_2d(box_II, R, R, prefix + "_mean_II", x, y);
        Func mean_p = box_filter_2d(box_p, R, R, prefix + "_mean_p", x, y);
        Func mean_Ip = box_filter_2d(box_Ip, R, R, prefix + "_mean_Ip", x, y);

        Func var_I(prefix + "_var_I"), cov_Ip(prefix + "_cov_Ip");
        var_I(x, y) = mean_II(x, y) - mean_I(x, y) * mean_I(x, y);
        cov_Ip(x, y) = mean_Ip(x, y) - mean_I(x, y) * mean_p(x, y);

        Func a(prefix + "_a"), b(prefix + "_b");
        a(x, y) = cov_Ip(x, y) / (var_I(x, y) + eps);
        b(x, y) = mean_p(x, y) - a(x, y) * mean_I(x, y);

        Func upsampled_a(prefix + "_upsampled_a"), upsampled_b(prefix + "_upsampled_b");
        upsampled_a(x, y) = a(x / s, y / s);
        upsampled_b(x, y) = b(x / s, y / s);
        
        // Schedule all intermediates to be inlined to form a single expression tree.
        small_guide.compute_inline();
        small_image.compute_inline();
        box_I.compute_inline();
        box_II.compute_inline();
        box_p.compute_inline();
        box_Ip.compute_inline();
        mean_I.compute_inline();
        mean_II.compute_inline();
        mean_p.compute_inline();
        mean_Ip.compute_inline();
        var_I.compute_inline();
        cov_Ip.compute_inline();
        a.compute_inline();
        b.compute_inline();
        upsampled_a.compute_inline();
        upsampled_b.compute_inline();

        output = Halide::Func(prefix + "_result");
        output(x, y) = upsampled_a(x, y) * guide(x, y) + upsampled_b(x, y);
        output.compute_inline();
    }
};

} // namespace


template <typename T>
class DenoiseBuilder_T {
public:
    Halide::Func output;

    DenoiseBuilder_T(Halide::Func input_raw,
                   Halide::Var x, Halide::Var y,
                   Halide::Expr strength,
                   Halide::Expr eps,
                   Halide::Expr blackLevel,
                   Halide::Expr whiteLevel,
                   const Halide::Target &target,
                   bool is_autoscheduled) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        // --- 1. Normalize input to float [0, 1] for internal calculations ---
        Func input_f("denoise_input_f");
        Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
        input_f(x, y) = (cast<float>(input_raw(x, y)) - blackLevel) * inv_range;
        
        // --- 2. Apply Variance-Stabilizing Transform (Anscombe) ---
        Func vst("vst");
        vst(x, y) = 2.0f * sqrt(max(0.0f, input_f(x, y)) + 3.0f/8.0f);

        // --- 3. Denoise in VST space using a Guided Filter with a FIXED radius ---
        const int fixed_radius = 2;
        GuidedFilter2DBuilder gf(vst, vst, x, y, fixed_radius, eps);

        Func denoised_vst("denoised_vst");
        denoised_vst(x, y) = gf.output(x, y);

        // --- 4. Apply Inverse VST ---
        Func inv_vst("inv_vst");
        Expr vst_val = denoised_vst(x, y);
        inv_vst(x, y) = (vst_val / 2.0f) * (vst_val / 2.0f) - 3.0f/8.0f;

        // --- 5. Blend with original based on strength ---
        Func blended("denoise_blended");
        blended(x, y) = lerp(input_f(x, y), inv_vst(x, y), strength);
        
        // Inline the entire chain of operations to create one large expression
        // for the final output. This is what allows specialize() to work.
        input_f.compute_inline();
        vst.compute_inline();
        denoised_vst.compute_inline();
        inv_vst.compute_inline();
        blended.compute_inline();

        // --- 6. Convert back to the pipeline's processing type ---
        output = Halide::Func("denoised_output");
        Expr final_val;
        if (std::is_same<T, float>::value) {
            // If the pipeline is float, the output should be a normalized float.
            final_val = blended(x, y);
        } else {
            // If the pipeline is uint16, denormalize back to the integer range.
            final_val = blended(x, y) * (cast<float>(whiteLevel) - cast<float>(blackLevel)) + blackLevel;
        }
        output(x, y) = proc_type_sat<T>(final_val);
    }
};

#endif // STAGE_DENOISE_H
