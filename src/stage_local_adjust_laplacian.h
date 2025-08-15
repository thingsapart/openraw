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
inline Halide::Func xyz_to_lab(Halide::Func xyz, Halide::Func lut, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
    Expr X_ = xyz(x, y, 0)/Xn, Y_ = xyz(x, y, 1)/Yn, Z_ = xyz(x, y, 2)/Zn;

    auto lut_lookup = [&](Expr val) {
        const int lut_size = 512;
        // The domain of lab_f is roughly [0, 2]. We map this to the LUT.
        Expr val_norm = val * (float)(lut_size - 1) / 2.0f;
        Expr val_idx = clamp(val_norm, 0.0f, lut_size - 2.0f);
        Expr vi = cast<int>(val_idx);
        Expr vf = val_idx - vi;
        return lerp(lut(vi), lut(vi + 1), vf);
    };

    Expr fX = lut_lookup(X_), fY = lut_lookup(Y_), fZ = lut_lookup(Z_);
    Expr L = 116.0f*fY - 16.0f, a = 500.0f*(fX - fY), b = 200.0f*(fY - fZ);
    Func lab(name); lab(x, y, c) = mux(c, {L, a, b}); return lab;
}
inline Halide::Expr lab_f_inv(Halide::Expr t) {
    using namespace Halide;
    const float delta = 6.0f / 29.0f;
    return select(t > delta, t*t*t, 3.0f*delta*delta*(t - 16.0f/116.0f));
}
inline Halide::Func lab_to_xyz(Halide::Func lab, Halide::Func lut, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
    Expr L = lab(x, y, 0), a = lab(x, y, 1), b = lab(x, y, 2);
    Expr fY = (L + 16.0f)/116.0f, fX = a/500.0f + fY, fZ = fY - b/200.0f;

    auto lut_lookup = [&](Expr val) {
        const int lut_size = 512;
        // The domain of lab_f_inv is roughly [-0.5, 1.5]. We map this to the LUT.
        Expr val_norm = (val + 0.5f) * (float)(lut_size - 1) / 2.0f;
        Expr val_idx = clamp(val_norm, 0.0f, lut_size - 2.0f);
        Expr vi = cast<int>(val_idx);
        Expr vf = val_idx - vi;
        return lerp(lut(vi), lut(vi + 1), vf);
    };

    Expr X = lut_lookup(fX)*Xn, Y = lut_lookup(fY)*Yn, Z = lut_lookup(fZ)*Zn;
    Func xyz(name); xyz(x, y, c) = mux(c, {X, Y, Z}); return xyz;
}
inline Halide::Func xyz_to_linear_srgb(Halide::Func xyz, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    Expr X = xyz(x, y, 0), Y = xyz(x, y, 1), Z = xyz(x, y, 2);
    Expr r = 3.2404542f*X - 1.5371385f*Y - 0.4985314f*Z, g = -0.9692660f*X + 1.8760108f*Y + 0.0415560f*Z, b = 0.0556434f*X - 0.2040259f*Y + 1.0572252f*Z;
    Func srgb(name); srgb(x, y, c) = mux(c, {r, g, b}); return srgb;
}
} // anonymous namespace

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

        downx(x, y, _) = (bounded_in(2*x - 1, y, _) + 3.f*bounded_in(2*x, y, _) + 3.f*bounded_in(2*x + 1, y, _) + bounded_in(2*x + 2, y, _)) / 8.f;

        // The intermediate `downx` is defined over the new, smaller width but original height.
        Expr w_out = (w_in + 1) / 2;
        Func bounded_downx = BoundaryConditions::repeat_edge(downx, {{Expr(0), w_out}, {Expr(0), h_in}});

        downy(x, y, _) = (bounded_downx(x, 2*y - 1, _) + 3.f*bounded_downx(x, 2*y, _) + 3.f*bounded_downx(x, 2*y + 1, _) + bounded_downx(x, 2*y + 2, _)) / 8.f;

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
        upx(x, y, _) = lerp(bounded_in(xi, y, _), bounded_in(xi + 1, y, _), xf - xi);

        // The intermediate `upx` is defined over the new, larger width but original height.
        Func bounded_upx = BoundaryConditions::repeat_edge(upx, {{Expr(0), w_out}, {Expr(0), h_in}});

        Expr yf = (cast<float>(y) / 2.0f) - 0.25f;
        Expr yi = cast<int>(floor(yf));
        upy(x, y, _) = lerp(bounded_upx(x, yi, _), bounded_upx(x, yi + 1, _), yf - yi);

        f_out = upy;
        intermediates.push_back(upx);
    }

public:
    Halide::Func output;
    std::vector<Halide::Func> gPyramid, inLPyramid, outGPyramid, outLPyramid;
    std::vector<Halide::Func> low_fi_intermediates, high_fi_intermediates, high_freq_pyramid_helpers, low_freq_pyramid_helpers, reconstruction_intermediates;
    Halide::Func remap_lut;
    Halide::Func lab_f_lut, lab_f_inv_lut;
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
                           Halide::Expr raw_width, Halide::Expr raw_height,
                           int J = 13, int cutover_level = 4)
        : output("local_adjustments_f"),
          gPyramid(J), inLPyramid(J), outGPyramid(J), outLPyramid(J),
          pyramid_levels(J)
    {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        // Create LUTs for the expensive Lab conversion functions
        lab_f_lut = Func("lab_f_lut");
        lab_f_inv_lut = Func("lab_f_inv_lut");
        Var i("lut_i");
        const int lut_size = 512;

        // lab_f domain is roughly [0, 2]. We map this to [0, lut_size-1]
        Expr t_f = cast<float>(i) * 2.0f / (lut_size - 1);
        lab_f_lut(i) = lab_f(t_f);

        // lab_f_inv domain is roughly [-0.5, 1.5]. We map this to [0, lut_size-1]
        Expr t_inv = (cast<float>(i) / (lut_size - 1)) * 2.0f - 0.5f;
        lab_f_inv_lut(i) = lab_f_inv(t_inv);

        Func input_f = input_rgb_normalized;

#ifndef BYPASS_LAPLACIAN_PYRAMID
        // --- PATH 1: High-Fidelity Path ---
        Func lab_hifi("lab_hifi"), L_norm_hifi("L_norm_hifi");
        Func a_chan("a_chan"), b_chan("b_chan");

        lab_hifi = xyz_to_lab(linear_srgb_to_xyz(input_f, x, y, c, "xyz_hifi"), lab_f_lut, x, y, c, "lab_hifi");
        L_norm_hifi(x, y) = lab_hifi(x, y, 0) / 100.0f;
        a_chan(x, y) = lab_hifi(x, y, 1); b_chan(x, y) = lab_hifi(x, y, 2);
        high_fi_intermediates = {lab_hifi, L_norm_hifi, a_chan, b_chan};

        // --- PATH 2: Low-Fidelity Path ---
        Func lowfi_spliced_L("lowfi_spliced_L");
        {
            Var hx("hx_lf"), hy("hy_lf");
            Func deinterleaved_raw = pipeline_deinterleave(raw_input, hx, hy, c);
            Func rgb_lowfi_sensor("rgb_lowfi_sensor");
            rgb_lowfi_sensor(hx,hy,c) = mux(c,{cast<float>(deinterleaved_raw(hx,hy,1)), avg(cast<float>(deinterleaved_raw(hx,hy,0)), cast<float>(deinterleaved_raw(hx,hy,3))), cast<float>(deinterleaved_raw(hx,hy,2))});

            // Fuse the deinterleave and simple demosaic steps by inlining them.
            deinterleaved_raw.compute_inline();
            rgb_lowfi_sensor.compute_inline();

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

            Func lowfi_rgb_downsampled = corrected_lowfi_norm;
            Expr current_w = raw_width / 2;
            Expr current_h = raw_height / 2;
            for (int j = 1; j < cutover_level; j++) {
                Func prev = lowfi_rgb_downsampled;
                downsample(prev, current_w, current_h, "lowfi_rgb_ds_"+std::to_string(j), lowfi_rgb_downsampled, low_freq_pyramid_helpers);
                current_w = (current_w + 1) / 2;
                current_h = (current_h + 1) / 2;
            }

            Var dx("dx_lf"), dy("dy_lf");
            Func lab_lowfi = xyz_to_lab(linear_srgb_to_xyz(lowfi_rgb_downsampled, dx, dy, c, "xyz_lowfi"), lab_f_lut, dx, dy, c, "lab_lowfi");
            lowfi_spliced_L(dx, dy) = lab_lowfi(dx, dy, 0) / 100.0f;

            low_fi_intermediates = {deinterleaved_raw, rgb_lowfi_sensor, corrected_lowfi_norm, lowfi_rgb_downsampled, lab_lowfi, lowfi_spliced_L};
        }

        // --- Build Input Pyramids (Gaussian and Laplacian) with Splicing ---
        std::vector<Expr> pyramid_widths(J);
        std::vector<Expr> pyramid_heights(J);
        pyramid_widths[0] = width;
        pyramid_heights[0] = height;
        for (int j = 1; j < J; j++) {
            pyramid_widths[j] = (pyramid_widths[j-1] + 1) / 2;
            pyramid_heights[j] = (pyramid_heights[j-1] + 1) / 2;
        }

        gPyramid[0] = L_norm_hifi;
        bool perform_splice = (cutover_level > 0 && cutover_level < pyramid_levels);

        if (perform_splice) {
            for (int j = 1; j < cutover_level; j++) {
                downsample(gPyramid[j-1], pyramid_widths[j-1], pyramid_heights[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], high_freq_pyramid_helpers);
            }
            gPyramid[cutover_level] = lowfi_spliced_L;
            for (int j = cutover_level + 1; j < pyramid_levels; j++) {
                downsample(gPyramid[j-1], pyramid_widths[j-1], pyramid_heights[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], low_freq_pyramid_helpers);
            }
        } else {
            for (int j = 1; j < pyramid_levels; j++) {
                downsample(gPyramid[j-1], pyramid_widths[j-1], pyramid_heights[j-1], "gpyr_ds_"+std::to_string(j), gPyramid[j], high_freq_pyramid_helpers);
            }
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

        // --- Correctly modify the Laplacian pyramid levels ---
        remap_lut = Func("remap_lut");
        Var i_remap("i_remap");
        Expr fi = cast<float>(i_remap) / 255.0f;
        Expr tonal_gain = (shadows/100.f)*(1.f-smoothstep(0.f,0.5f,fi)) + (highlights/100.f)*smoothstep(0.5f,1.f,fi);
        remap_lut(i_remap) = fast_pow(fi, fast_pow(2.f, -tonal_gain));

        Expr clarity_gain = 1.0f + clarity / 100.0f;

        int base_level = pyramid_levels - 1;
        if (base_level >= 0) {
            outLPyramid[base_level] = Func("outLPyramid_" + std::to_string(base_level));
            Expr g_base = gPyramid[base_level](x,y);
            Expr idx = cast<int>(clamp(g_base * 255.0f, 0.f, 255.f));
            outLPyramid[base_level](x,y) = remap_lut(idx);
        }

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
                upsample(outGPyramid[j+1], pyramid_widths[j+1], pyramid_heights[j+1], pyramid_widths[j], pyramid_heights[j], "outGpyr_us_"+std::to_string(j), upsampled_out, helpers);
                outGPyramid[j](x,y) = upsampled_out(x,y) + outLPyramid[j](x,y);
            }
        }

        // --- Convert back to RGB ---
        Func L_out("L_out"), lab_out("lab_out"), xyz_out("xyz_out"), rgb_out_f("rgb_out_f"), final_adjustments("final_adjustments");

        if (pyramid_levels > 0) {
            L_out(x, y) = outGPyramid[0](x, y) * 100.0f;
        } else {
            L_out(x, y) = L_norm_hifi(x,y) * 100.0f;
        }

        lab_out(x, y, c) = mux(c, {L_out(x, y), a_chan(x, y), b_chan(x, y)});
        xyz_out = lab_to_xyz(lab_out, lab_f_inv_lut, x, y, c, "final_xyz");
        rgb_out_f = xyz_to_linear_srgb(xyz_out, x, y, c, "final_rgb_f");
        Expr blacks_level = blacks/250.f, whites_level = 1.f + whites/250.f;
        Expr remap_denom = whites_level - blacks_level;
        final_adjustments(x,y,c) = (rgb_out_f(x,y,c) - blacks_level) / select(abs(remap_denom)<1e-5f, 1e-5f, remap_denom);
        reconstruction_intermediates = {L_out, lab_out, xyz_out, rgb_out_f, final_adjustments};

        Expr is_default = clarity == 0 && shadows == 0 && highlights == 0 && blacks == 0 && whites == 0;
        output(x, y, c) = select(is_default, input_f(x, y, c), final_adjustments(x, y, c));

#else
        // --- DEBUG PATH: BYPASS PYRAMID ---
        Func xyz_fwd = linear_srgb_to_xyz(input_f, x, y, c, "bypass_xyz_fwd");
        Func lab = xyz_to_lab(xyz_fwd, lab_f_lut, x, y, c, "bypass_lab");
        Func xyz_bwd = lab_to_xyz(lab, lab_f_inv_lut, x, y, c, "bypass_xyz_bwd");
        Func rgb_out_f = xyz_to_linear_srgb(xyz_bwd, x, y, c, "bypass_rgb_out");
        high_fi_intermediates = {input_f, xyz_fwd, lab, xyz_bwd, rgb_out_f};
        output(x, y, c) = rgb_out_f(x, y, c);
#endif
    }
};

#endif // STAGE_LOCAL_ADJUST_LAPLACIAN_H
