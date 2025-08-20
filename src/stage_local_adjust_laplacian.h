#ifndef STAGE_LOCAL_ADJUST_LAPLACIAN_H
#define STAGE_LOCAL_ADJUST_LAPLACIAN_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include "stage_resize.h"
#include "stage_deinterleave.h"
#include "stage_bayer_normalize.h"
#include "color_tools.h"
#include <vector>
#include <string>
#include <type_traits>
#include <memory>

// To debug this stage, define this macro during compilation.
// This will bypass the pyramid logic and only perform the color space conversions.
// e.g., cmake -S . -B build -DBYPASS_LAPLACIAN_PYRAMID=ON
// #define BYPASS_LAPLACIAN_PYRAMID

class LocalLaplacianBuilder {
private:
    // Self-contained, safe pyramid helpers. They explicitly use boundary
    // conditions to handle small images and allow for pyramid levels that are
    // smaller than the filter kernels.
    static void downsample(Halide::Func f_in, Halide::Expr w_in, Halide::Expr h_in,
                           const std::string& name_prefix,
                           Halide::Func &f_out, std::vector<Halide::Func> &intermediates) {
        using namespace Halide;
        Func bounded_in = BoundaryConditions::repeat_edge(f_in, {{Expr(0), w_in}, {Expr(0), h_in}});

        Func downx(name_prefix + "_downx"), downy(name_prefix + "_downy");
        Var x, y;

        downx(x, y) = (bounded_in(2*x - 1, y) + 3.f*bounded_in(2*x, y) + 3.f*bounded_in(2*x + 1, y) + bounded_in(2*x + 2, y)) / 8.f;
        Expr w_out = (w_in + 1) / 2;
        Func bounded_downx = BoundaryConditions::repeat_edge(downx, {{Expr(0), w_out}, {Expr(0), h_in}});

        downy(x, y) = (bounded_downx(x, 2*y - 1) + 3.f*bounded_downx(x, 2*y) + 3.f*bounded_downx(x, 2*y + 1) + bounded_downx(x, 2*y + 2)) / 8.f;

        f_out = downy;
        intermediates.push_back(downx);
    }

    static void upsample(Halide::Func f_in, Halide::Expr w_in, Halide::Expr h_in, Halide::Expr w_out, Halide::Expr h_out,
                         const std::string& name_prefix,
                         Halide::Func &f_out, std::vector<Halide::Func> &intermediates) {
        using namespace Halide;
        Func bounded_in = BoundaryConditions::repeat_edge(f_in, {{Expr(0), w_in}, {Expr(0), h_in}});

        Func upx(name_prefix + "_upx"), upy(name_prefix + "_upy");
        Var x, y;

        Expr xf = (cast<float>(x) / 2.0f) - 0.25f;
        Expr xi = cast<int>(floor(xf));
        upx(x, y) = lerp(bounded_in(xi, y), bounded_in(xi + 1, y), xf - xi);
        Func bounded_upx = BoundaryConditions::repeat_edge(upx, {{Expr(0), w_out}, {Expr(0), h_in}});

        Expr yf = (cast<float>(y) / 2.0f) - 0.25f;
        Expr yi = cast<int>(floor(yf));
        upy(x, y) = lerp(bounded_upx(x, yi), bounded_upx(x, yi + 1), yf - yi);

        f_out = upy;
        intermediates.push_back(upx);
    }

public:
    Halide::Func output;
    std::vector<Halide::Func> gPyramid, inLPyramid, outLPyramid;
    std::vector<std::vector<Halide::Func>> reconstructedGPyramid; // For dynamic reconstruction
    std::vector<Halide::Func> low_fi_intermediates, high_fi_intermediates, high_freq_pyramid_helpers, low_freq_pyramid_helpers, reconstruction_intermediates;
    Halide::Func remap_lut;
    std::unique_ptr<ResizeBicubicBuilder> lowfi_resize_builder;
    const int pyramid_levels;

    LocalLaplacianBuilder(Halide::Func input_lch,
                           Halide::Func raw_input,
                           Halide::Func cc_matrix,
                           Halide::Expr cfa_pattern,
                           Halide::Expr wb_r_gain, Halide::Expr wb_g_gain, Halide::Expr wb_b_gain, Halide::Expr green_balance, Halide::Expr exposure_multiplier,
                           Halide::Var x, Halide::Var y, Halide::Var c,
                           Halide::Expr detail_sharpen, Halide::Expr clarity,
                           Halide::Expr shadows, Halide::Expr highlights,
                           Halide::Expr blacks, Halide::Expr whites,
                           Halide::Expr debug_level,
                           Halide::Expr blackLevel, Halide::Expr whiteLevel,
                           Halide::Expr width, Halide::Expr height,
                           Halide::Expr raw_width, Halide::Expr raw_height,
                           Halide::Expr downscale_factor,
                           int J = 8, int cutover_level = 4)
        : output("lch_local_adjusted"),
          gPyramid(J), inLPyramid(J), outLPyramid(J),
          reconstructedGPyramid(J, std::vector<Halide::Func>(J)),
          lowfi_resize_builder(nullptr),
          pyramid_levels(J)
    {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

#ifndef BYPASS_LAPLACIAN_PYRAMID
        // --- PATH 1: High-Fidelity Path ---
        // The input is LCH. We operate on the L channel and pass C,h through.
        Func L_in("L_in"), C_in("C_in"), h_in("h_in"), L_norm_hifi("L_norm_hifi");
        L_in(x, y) = input_lch(x, y, 0);
        C_in(x, y) = input_lch(x, y, 1);
        h_in(x, y) = input_lch(x, y, 2);
        L_norm_hifi(x, y) = L_in(x, y) / 100.0f;
        high_fi_intermediates = {L_in, C_in, h_in, L_norm_hifi};

        // --- PATH 2: Low-Fidelity Path ---
        Func lowfi_spliced_L("lowfi_spliced_L");
        {
            Var hx("hx"), hy("hy"); // Use distinct Var names
            // 1. Replicate the main pipeline's initial normalization and exposure step.
            Func linear_exposed_lowfi("linear_exposed_lowfi");
            Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
            linear_exposed_lowfi(hx, hy) = (cast<float>(raw_input(hx, hy)) - cast<float>(blackLevel)) * inv_range * exposure_multiplier;
            low_fi_intermediates.push_back(linear_exposed_lowfi);

            // 2. Replicate the main pipeline's white balancing step.
            BayerNormalizeBuilder normalize_builder_lowfi(linear_exposed_lowfi, cfa_pattern, green_balance, wb_r_gain, wb_g_gain, wb_b_gain, hx, hy);
            Func normalized_bayer_lowfi = normalize_builder_lowfi.output;
            low_fi_intermediates.push_back(normalized_bayer_lowfi);

            // 3. Deinterleave the now-canonical GRBG bayer data.
            Func deinterleaved_lowfi = pipeline_deinterleave(normalized_bayer_lowfi, hx, hy, c);
            low_fi_intermediates.push_back(deinterleaved_lowfi);
            
            // 4. Perform a cheap demosaic.
            Func rgb_lowfi_sensor("rgb_lowfi_sensor");
            rgb_lowfi_sensor(hx,hy,c) = mux(c,{
                deinterleaved_lowfi(hx,hy,1),
                avg(deinterleaved_lowfi(hx,hy,0), deinterleaved_lowfi(hx,hy,3)),
                deinterleaved_lowfi(hx,hy,2)
            });
            low_fi_intermediates.push_back(rgb_lowfi_sensor);

            // 5. Apply the color matrix.
            Expr ir_sensor = rgb_lowfi_sensor(hx,hy,0), ig_sensor = rgb_lowfi_sensor(hx,hy,1), ib_sensor = rgb_lowfi_sensor(hx,hy,2);
            Expr r_f_sensor = cc_matrix(3, 0) + cc_matrix(0, 0) * ir_sensor + cc_matrix(1, 0) * ig_sensor + cc_matrix(2, 0) * ib_sensor;
            Expr g_f_sensor = cc_matrix(3, 1) + cc_matrix(0, 1) * ir_sensor + cc_matrix(1, 1) * ig_sensor + cc_matrix(2, 1) * ib_sensor;
            Expr b_f_sensor = cc_matrix(3, 2) + cc_matrix(0, 2) * ir_sensor + cc_matrix(1, 2) * ig_sensor + cc_matrix(2, 2) * ib_sensor;

            Func corrected_lowfi_norm("corrected_lowfi_norm");
            corrected_lowfi_norm(hx,hy,c) = clamp(mux(c, {r_f_sensor, g_f_sensor, b_f_sensor}), 0.0f, 1.0f);
            low_fi_intermediates.push_back(corrected_lowfi_norm);

            // 6. Optional downscaling to match the main pipeline's output resolution.
            Func lowfi_rgb_maybe_downscaled("lowfi_rgb_maybe_downscaled");
            Expr is_no_op = abs(downscale_factor - 1.0f) < 1e-6f;
            Expr lowfi_w = raw_width / 2, lowfi_h = raw_height / 2;
            Expr lowfi_down_w = cast<int>(lowfi_w / downscale_factor), lowfi_down_h = cast<int>(lowfi_h / downscale_factor);
            lowfi_resize_builder = std::make_unique<ResizeBicubicBuilder>(corrected_lowfi_norm, "lowfi_resize", lowfi_w, lowfi_h, lowfi_down_w, lowfi_down_h, hx, hy, c);
            low_fi_intermediates.push_back(lowfi_resize_builder->output);
            lowfi_rgb_maybe_downscaled(hx, hy, c) = select(is_no_op, corrected_lowfi_norm(hx, hy, c), lowfi_resize_builder->output(hx, hy, c));

            Expr current_w = select(is_no_op, lowfi_w, lowfi_down_w), current_h = select(is_no_op, lowfi_h, lowfi_down_h);

            // 7. Convert the now-correct sRGB image to LCH to get the luminance signal.
            Var dx("dx_lf"), dy("dy_lf"), dc("dc_lf");
            Func lch_lowfi = HalideColor::linear_srgb_to_lch(lowfi_rgb_maybe_downscaled, dx, dy, dc);
            low_fi_intermediates.push_back(lch_lowfi);

            Func L_lowfi("L_lowfi");
            L_lowfi(dx, dy) = lch_lowfi(dx, dy, 0);

            // 8. Downsample the L channel to the splice level for the pyramid.
            Func L_lowfi_downsampled = L_lowfi;
            for (int j = 1; j < cutover_level; j++) {
                Func prev = L_lowfi_downsampled;
                downsample(prev, current_w, current_h, "lowfi_L_ds_"+std::to_string(j), L_lowfi_downsampled, low_freq_pyramid_helpers);
                current_w = (current_w + 1) / 2;
                current_h = (current_h + 1) / 2;
            }

            lowfi_spliced_L(dx, dy) = L_lowfi_downsampled(dx, dy) / 100.f;
            low_fi_intermediates.push_back(L_lowfi);
            low_fi_intermediates.push_back(lowfi_spliced_L);
        }

        // --- Build Input Pyramids (Gaussian and Laplacian) with Splicing ---
        std::vector<Expr> pyramid_widths(J); std::vector<Expr> pyramid_heights(J);
        pyramid_widths[0] = width; pyramid_heights[0] = height;
        for (int j = 1; j < J; j++) { pyramid_widths[j] = (pyramid_widths[j-1] + 1) / 2; pyramid_heights[j] = (pyramid_heights[j-1] + 1) / 2; }

        gPyramid[0] = L_norm_hifi;
        bool perform_splice = (cutover_level > 0 && cutover_level < pyramid_levels);

        if (perform_splice) {
            for (int j = 1; j < cutover_level; j++) downsample(gPyramid[j-1], pyramid_widths[j-1], pyramid_heights[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], high_freq_pyramid_helpers);
            gPyramid[cutover_level] = lowfi_spliced_L;
            for (int j = cutover_level + 1; j < pyramid_levels; j++) downsample(gPyramid[j-1], pyramid_widths[j-1], pyramid_heights[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], low_freq_pyramid_helpers);
        } else {
            for (int j = 1; j < pyramid_levels; j++) downsample(gPyramid[j-1], pyramid_widths[j-1], pyramid_heights[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], high_freq_pyramid_helpers);
        }

        if (pyramid_levels > 0) {
            inLPyramid[pyramid_levels-1] = gPyramid[pyramid_levels-1];
            for(int j=pyramid_levels-2; j>=0; j--) {
                inLPyramid[j] = Func("inLPyramid_"+std::to_string(j));
                Func upsampled_g;
                auto& helpers = (perform_splice && j >= cutover_level-1) ? low_freq_pyramid_helpers : high_freq_pyramid_helpers;
                upsample(gPyramid[j+1], pyramid_widths[j+1], pyramid_heights[j+1], pyramid_widths[j], pyramid_heights[j], "inLpyr_us_"+std::to_string(j), upsampled_g, helpers);
                inLPyramid[j](x,y) = gPyramid[j](x,y) - upsampled_g(x,y);
            }
        }

        remap_lut = Func("remap_lut");
        Var i_remap("i_remap");
        Expr fi = cast<float>(i_remap) / 255.0f;
        Expr tonal_gain = (shadows/100.f)*(1.f-smoothstep(0.f,0.5f,fi)) + (highlights/100.f)*smoothstep(0.5f,1.f,fi);
        remap_lut(i_remap) = fast_pow(fi, fast_pow(2.f, -tonal_gain));
        Expr detail_gain = 1.0f + detail_sharpen / 100.0f; Expr clarity_gain = 1.0f + clarity / 100.0f;
        const int detail_level_cutoff = 2, clarity_level_cutoff = 5;

        for(int j=0; j < pyramid_levels; j++) {
            outLPyramid[j] = Func("outLPyramid_"+std::to_string(j));
            Expr adjusted_laplacian = inLPyramid[j](x,y) * select(j < detail_level_cutoff, detail_gain, select(j < clarity_level_cutoff, clarity_gain, 1.0f));

            // If debug_level is active (>0), zero out the finest levels.
            outLPyramid[j](x,y) = select(debug_level > 0 && j < (pyramid_levels - debug_level),
                                         0.0f,
                                         adjusted_laplacian);
        }

        const float target_sh_size = 256.0f;
        Expr j_sh = clamp(cast<int>(round(fast_log(cast<float>(max(width, height))/target_sh_size) / fast_log(2.f))), 0, pyramid_levels - 1);
        std::vector<Func> final_reconstructions(J);
        for (int b = 0; b < J; ++b) {
            reconstructedGPyramid[b][b] = Func("reconstructedG_" + std::to_string(b) + "_base");
            reconstructedGPyramid[b][b](x,y) = remap_lut(cast<int>(clamp(gPyramid[b](x,y) * 255.0f, 0.f, 255.f)));
            for (int j = b - 1; j >= 0; j--) {
                reconstructedGPyramid[b][j] = Func("reconstructedG_" + std::to_string(b) + "_" + std::to_string(j));
                Func upsampled_out;
                auto& helpers = (perform_splice && j >= cutover_level-1) ? low_freq_pyramid_helpers : high_freq_pyramid_helpers;
                upsample(reconstructedGPyramid[b][j+1], pyramid_widths[j+1], pyramid_heights[j+1], pyramid_widths[j], pyramid_heights[j], "reconG_us_"+std::to_string(b)+"_"+std::to_string(j), upsampled_out, helpers);
                reconstructedGPyramid[b][j](x,y) = upsampled_out(x,y) + outLPyramid[j](x,y);
            }
            final_reconstructions[b] = reconstructedGPyramid[b][0];
        }

        Func L_out_norm("L_out_norm");
        Expr final_L_expr_norm = (J > 0) ? final_reconstructions[J-1](x,y) : L_norm_hifi(x,y);
        for (int b = J-2; b >= 0; b--) final_L_expr_norm = select(j_sh == b, final_reconstructions[b](x,y), final_L_expr_norm);
        L_out_norm(x, y) = final_L_expr_norm;

        Func L_out("L_out");
        Expr blacks_level = blacks/100.f * 0.5f, whites_level = 1.f - whites/100.f * 0.5f;
        Expr remap_denom = whites_level - blacks_level;
        Expr L_remapped = (L_out_norm(x,y) - blacks_level) / select(abs(remap_denom)<1e-5f, 1e-5f, remap_denom);
        L_out(x,y) = clamp(L_remapped, 0.f, 1.f) * 100.f; // Clamp normalized result before scaling
        reconstruction_intermediates = {L_out_norm, L_out};

        Expr is_default = detail_sharpen == 0 && clarity == 0 && shadows == 0 && highlights == 0 && blacks == 0 && whites == 0 && debug_level <= 0;
        output(x, y, c) = mux(c, { select(is_default, L_in(x,y), L_out(x,y)), C_in(x,y), h_in(x,y) });
#else
        output(x, y, c) = input_lch(x, y, c);
#endif
    }
};

#endif // STAGE_LOCAL_ADJUST_LAPLACIAN_H

