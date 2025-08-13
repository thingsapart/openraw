#ifndef STAGE_LOCAL_ADJUST_LAPLACIAN_H
#define STAGE_LOCAL_ADJUST_LAPLACIAN_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include "stage_deinterleave.h"
#include <vector>
#include <string>
#include <type_traits>

// To debug this stage, define this macro during compilation.
// This will bypass the pyramid logic and only perform the color space conversions.
// e.g., cmake -S . -B build -DBYPASS_LAPLACIAN_PYRAMID=ON
// #define BYPASS_LAPLACIAN_PYRAMID

namespace {
// This version of srgb_to_xyz is for LINEAR sRGB input.
inline Halide::Func linear_srgb_to_xyz(Halide::Func srgb_linear, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    Expr r = srgb_linear(x, y, 0); Expr g = srgb_linear(x, y, 1); Expr b = srgb_linear(x, y, 2);
    Expr X = 0.4124564f*r + 0.3575761f*g + 0.1804375f*b, Y = 0.2126729f*r + 0.7151522f*g + 0.0721750f*b, Z = 0.0193339f*r + 0.1191920f*g + 0.9503041f*b;
    Func xyz(name); xyz(x, y, c) = mux(c, {X, Y, Z}); return xyz;
}
inline Halide::Expr lab_f(Halide::Expr t) {
    using namespace Halide;
    const float t_thresh = 0.008856451679035631f;
    return select(t > t_thresh, Halide::fast_pow(t, 1.0f / 3.0f), (7.787037037037037f * t) + (16.0f / 116.0f));
}
inline Halide::Func xyz_to_lab(Halide::Func xyz, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
    Expr X_ = xyz(x, y, 0)/Xn, Y_ = xyz(x, y, 1)/Yn, Z_ = xyz(x, y, 2)/Zn;
    Expr fX = lab_f(X_), fY = lab_f(Y_), fZ = lab_f(Z_);
    Expr L = 116.0f*fY - 16.0f, a = 500.0f*(fX - fY), b = 200.0f*(fY - fZ);
    Func lab(name); lab(x, y, c) = mux(c, {L, a, b}); return lab;
}
inline Halide::Expr lab_f_inv(Halide::Expr t) {
    using namespace Halide;
    const float delta = 6.0f / 29.0f;
    return select(t > delta, t*t*t, 3.0f*delta*delta*(t - 16.0f/116.0f));
}
inline Halide::Func lab_to_xyz(Halide::Func lab, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
    Expr L = lab(x, y, 0), a = lab(x, y, 1), b = lab(x, y, 2);
    Expr fY = (L + 16.0f)/116.0f, fX = a/500.0f + fY, fZ = fY - b/200.0f;
    Expr X = lab_f_inv(fX)*Xn, Y = lab_f_inv(fY)*Yn, Z = lab_f_inv(fZ)*Zn;
    Func xyz(name); xyz(x, y, c) = mux(c, {X, Y, Z}); return xyz;
}
inline Halide::Func xyz_to_linear_srgb(Halide::Func xyz, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    Expr X = xyz(x, y, 0), Y = xyz(x, y, 1), Z = xyz(x, y, 2);
    Expr r = 3.2404542f*X - 1.5371385f*Y - 0.4985314f*Z, g = -0.9692660f*X + 1.8760108f*Y + 0.0415560f*Z, b = 0.0556434f*X - 0.2040259f*Y + 1.0572252f*Z;
    Func srgb(name); srgb(x, y, c) = mux(c, {r, g, b}); return srgb;
}
void downsample(Halide::Func f, const std::string& name_prefix, Halide::Func &output, std::vector<Halide::Func> &intermediates) {
    using namespace Halide;
    Func downx(name_prefix + "_downx"), downy(name_prefix + "_downy");
    Var x, y;
    downx(x, y, _) = (f(2*x - 1, y, _) + 3.f*f(2*x, y, _) + 3.f*f(2*x + 1, y, _) + f(2*x + 2, y, _)) / 8.f;
    downy(x, y, _) = (downx(x, 2*y - 1, _) + 3.f*downx(x, 2*y, _) + 3.f*downx(x, 2*y + 1, _) + downx(x, 2*y + 2, _)) / 8.f;
    output = downy;
    intermediates.push_back(downx);
}
void upsample(Halide::Func f, const std::string& name_prefix, Halide::Func &output, std::vector<Halide::Func> &intermediates) {
    using namespace Halide;
    Func upx(name_prefix + "_upx"), upy(name_prefix + "_upy");
    Var x, y;
    Expr xf = (cast<float>(x) + 0.5f) / 2.0f; Expr xi = cast<int>(xf);
    upx(x, y, _) = lerp(f(xi, y, _), f(xi + 1, y, _), xf - xi);
    Expr yf = (cast<float>(y) + 0.5f) / 2.0f; Expr yi = cast<int>(yf);
    upy(x, y, _) = lerp(upx(x, yi, _), upx(x, yi + 1, _), yf - yi);
    output = upy;
    intermediates.push_back(upx);
}
} // anonymous namespace

class LocalLaplacianBuilder {
public:
    Halide::Func output;
    std::vector<Halide::Func> gPyramid, inLPyramid, outGPyramid, outLPyramid;
    std::vector<Halide::Func> low_fi_intermediates, high_fi_intermediates, high_freq_pyramid_helpers, low_freq_pyramid_helpers, reconstruction_intermediates;
    Halide::Func remap_lut;
    const int pyramid_levels;

    LocalLaplacianBuilder(Halide::Func input_rgb_normalized,
                           Halide::Func raw_input,
                           Halide::Func cc_matrix,
                           Halide::Var x, Halide::Var y, Halide::Var c,
                           Halide::Expr detail_sharpen, Halide::Expr clarity,
                           Halide::Expr shadows, Halide::Expr highlights,
                           Halide::Expr blacks, Halide::Expr whites,
                           Halide::Expr blackLevel, Halide::Expr whiteLevel,
                           Halide::Expr width, Halide::Expr height,
                           int J = 8, int cutover_level = 4)
        : output("local_adjustments_f"),
          gPyramid(J), inLPyramid(J), outGPyramid(J), outLPyramid(J),
          pyramid_levels(J)
    {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        Func input_f = input_rgb_normalized;

#ifndef BYPASS_LAPLACIAN_PYRAMID
        // --- PATH 1: High-Fidelity Path ---
        Func lab_hifi("lab_hifi"), L_norm_hifi("L_norm_hifi");
        Func a_chan("a_chan"), b_chan("b_chan");

        lab_hifi = xyz_to_lab(linear_srgb_to_xyz(input_f, x, y, c, "xyz_hifi"), x, y, c, "lab_hifi");
        L_norm_hifi(x, y) = lab_hifi(x, y, 0) / 100.0f;
        a_chan(x, y) = lab_hifi(x, y, 1); b_chan(x, y) = lab_hifi(x, y, 2);
        high_fi_intermediates = {lab_hifi, L_norm_hifi, a_chan, b_chan};

        // --- PATH 2: Low-Fidelity Path ---
        // This path produces a half-resolution L* image, equivalent in size
        // and content to gPyramid[1] from the hi-fi path.
        Func L_norm_lowfi_halfres("L_norm_lowfi_halfres");
        {
            Var hx("hx_lf"), hy("hy_lf"); // Local vars for the half-resolution grid.
            Func deinterleaved_raw = pipeline_deinterleave(raw_input, hx, hy, c);
            Func rgb_lowfi_sensor("rgb_lowfi_sensor");
            rgb_lowfi_sensor(hx,hy,c) = mux(c,{cast<float>(deinterleaved_raw(hx,hy,1)), avg(cast<float>(deinterleaved_raw(hx,hy,0)), cast<float>(deinterleaved_raw(hx,hy,3))), cast<float>(deinterleaved_raw(hx,hy,2))});

            // Mimic the main color correction stage: apply matrix to sensor-space data, then normalize.
            Expr ir_sensor = rgb_lowfi_sensor(hx,hy,0);
            Expr ig_sensor = rgb_lowfi_sensor(hx,hy,1);
            Expr ib_sensor = rgb_lowfi_sensor(hx,hy,2);
            Expr r_f_sensor = cc_matrix(3, 0) + cc_matrix(0, 0) * ir_sensor + cc_matrix(1, 0) * ig_sensor + cc_matrix(2, 0) * ib_sensor;
            Expr g_f_sensor = cc_matrix(3, 1) + cc_matrix(0, 1) * ir_sensor + cc_matrix(1, 1) * ig_sensor + cc_matrix(2, 1) * ib_sensor;
            Expr b_f_sensor = cc_matrix(3, 2) + cc_matrix(0, 2) * ir_sensor + cc_matrix(1, 2) * ig_sensor + cc_matrix(2, 2) * ib_sensor;

            Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
            Func corrected_lowfi_norm("corrected_lowfi_norm");
            corrected_lowfi_norm(hx,hy,c) = mux(c, {
                (r_f_sensor - blackLevel) * inv_range,
                (g_f_sensor - blackLevel) * inv_range,
                (b_f_sensor - blackLevel) * inv_range
            });

            Func lab_lowfi = xyz_to_lab(linear_srgb_to_xyz(corrected_lowfi_norm, hx, hy, c, "xyz_lowfi"), hx, hy, c, "lab_lowfi");
            L_norm_lowfi_halfres(hx, hy) = lab_lowfi(hx, hy, 0) / 100.0f;
            low_fi_intermediates = {deinterleaved_raw, rgb_lowfi_sensor, corrected_lowfi_norm, lab_lowfi, L_norm_lowfi_halfres};
        }

        // --- Build Input Pyramids (Gaussian and Laplacian) with Splicing ---
        gPyramid[0] = L_norm_hifi;

        // The splice only makes sense if there are at least two levels, and the
        // cutover is strictly between the first and last level.
        bool perform_splice = (cutover_level > 0 && cutover_level < pyramid_levels);

        if (perform_splice) {
            // Build hi-fi path up to the splice point
            for (int j = 1; j < cutover_level; j++) {
                downsample(gPyramid[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], high_freq_pyramid_helpers);
            }

            // Prepare the low-fi branch for splicing.
            // The low-fi path starts at half-resolution, equivalent to pyramid level 1.
            // We need to downsample it (cutover_level - 1) more times to match gPyramid[cutover_level].
            Func lowfi_spliced = L_norm_lowfi_halfres;
            for (int j = 1; j < cutover_level; j++) {
                Func prev = lowfi_spliced;
                downsample(prev, "lowfi_ds_"+std::to_string(j), lowfi_spliced, low_freq_pyramid_helpers);
            }

            // Perform the splice.
            gPyramid[cutover_level] = lowfi_spliced;

            // Continue building the pyramid from the spliced-in low-fi branch.
            for (int j = cutover_level + 1; j < pyramid_levels; j++) {
                downsample(gPyramid[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], low_freq_pyramid_helpers);
            }
        } else {
            // No splice, just build a standard Gaussian pyramid from the hi-fi source.
            for (int j = 1; j < pyramid_levels; j++) {
                downsample(gPyramid[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], high_freq_pyramid_helpers);
            }
        }

        if (pyramid_levels > 0) {
            inLPyramid[pyramid_levels-1] = gPyramid[pyramid_levels-1];
            for(int j=pyramid_levels-2; j>=0; j--) {
                inLPyramid[j] = Func("inLPyramid_"+std::to_string(j));
                Func upsampled_g;
                auto& helpers = (perform_splice && j >= cutover_level-1) ? low_freq_pyramid_helpers : high_freq_pyramid_helpers;
                upsample(gPyramid[j+1], "inLpyr_us_"+std::to_string(j), upsampled_g, helpers);
                inLPyramid[j](x,y) = gPyramid[j](x,y) - upsampled_g(x,y);
            }
        }

        // --- Correctly modify the Laplacian pyramid levels ---
        remap_lut = Func("remap_lut");
        Var i("i_remap");
        Expr fi = cast<float>(i) / 255.0f;
        Expr tonal_gain = (shadows/100.f)*(1.f-smoothstep(0.f,0.5f,fi)) + (highlights/100.f)*smoothstep(0.5f,1.f,fi);
        // The LUT computes a remapped intensity value, e.g., using a gamma curve.
        remap_lut(i) = fast_pow(fi, fast_pow(2.f, -tonal_gain));

        Expr clarity_gain = 1.0f + clarity / 100.0f;

        // Apply tonal adjustments only to the base layer.
        int base_level = pyramid_levels - 1;
        if (base_level >= 0) {
            outLPyramid[base_level] = Func("outLPyramid_" + std::to_string(base_level));
            Expr g_base = gPyramid[base_level](x,y);
            Expr idx = cast<int>(clamp(g_base * 255.0f, 0.f, 255.f));
            outLPyramid[base_level](x,y) = remap_lut(idx);
        }

        // Apply clarity adjustments only to the detail/residual layers.
        for(int j=0; j < base_level; j++) {
            outLPyramid[j] = Func("outLPyramid_"+std::to_string(j));
            outLPyramid[j](x,y) = inLPyramid[j](x,y) * clarity_gain;
        }

        // --- Reconstruct the output Gaussian pyramid ---
        if (pyramid_levels > 0) {
            outGPyramid[pyramid_levels-1] = outLPyramid[pyramid_levels-1];
            for(int j=pyramid_levels-2; j>=0; j--) {
                outGPyramid[j] = Func("outGPyramid_"+std::to_string(j));
                Func upsampled_out;
                auto& helpers = (perform_splice && j >= cutover_level-1) ? low_freq_pyramid_helpers : high_freq_pyramid_helpers;
                upsample(outGPyramid[j+1], "outGpyr_us_"+std::to_string(j), upsampled_out, helpers);
                outGPyramid[j](x,y) = upsampled_out(x,y) + outLPyramid[j](x,y);
            }
        }

        // --- Convert back to RGB ---
        Func L_out("L_out"), lab_out("lab_out"), xyz_out("xyz_out"), rgb_out_f("rgb_out_f"), final_adjustments("final_adjustments");

        if (pyramid_levels > 0) {
            L_out(x, y) = outGPyramid[0](x, y) * 100.0f;
        } else {
            // Handle J=0 case, though it's unlikely. Just pass through Luma.
            L_out(x, y) = L_norm_hifi(x,y) * 100.0f;
        }

        lab_out(x, y, c) = mux(c, {L_out(x, y), a_chan(x, y), b_chan(x, y)});
        xyz_out = lab_to_xyz(lab_out, x, y, c, "final_xyz");
        rgb_out_f = xyz_to_linear_srgb(xyz_out, x, y, c, "final_rgb_f");
        Expr blacks_level = blacks/250.f, whites_level = 1.f + whites/250.f;
        Expr remap_denom = whites_level - blacks_level;
        final_adjustments(x,y,c) = (rgb_out_f(x,y,c) - blacks_level) / select(abs(remap_denom)<1e-5f, 1e-5f, remap_denom);
        reconstruction_intermediates = {L_out, lab_out, xyz_out, rgb_out_f, final_adjustments};

        Expr is_default = clarity == 0 && shadows == 0 && highlights == 0 && blacks == 0 && whites == 0;
        output(x, y, c) = select(is_default, input_f(x, y, c), final_adjustments(x, y, c));

#else
        // --- DEBUG PATH: BYPASS PYRAMID ---
        // This path tests only the color space conversions.

        // Round trip through the color spaces
        Func xyz_fwd = linear_srgb_to_xyz(input_f, x, y, c, "bypass_xyz_fwd");
        Func lab = xyz_to_lab(xyz_fwd, x, y, c, "bypass_lab");
        Func xyz_bwd = lab_to_xyz(lab, x, y, c, "bypass_xyz_bwd");
        Func rgb_out_f = xyz_to_linear_srgb(xyz_bwd, x, y, c, "bypass_rgb_out");

        // Store all intermediates so we can inspect them if needed.
        high_fi_intermediates = {input_f, xyz_fwd, lab, xyz_bwd, rgb_out_f};

        // The output is always float [0,1]
        output(x, y, c) = rgb_out_f(x, y, c);
#endif
    }
};

#endif // STAGE_LOCAL_ADJUST_LAPLACIAN_H
