#ifndef STAGE_LOCAL_ADJUST_LAPLACIAN_H
#define STAGE_LOCAL_ADJUST_LAPLACIAN_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include <vector>
#include <string>
#include <type_traits>

// This file implements a truly forked Local Laplacian Filter.

namespace {

// --- NEW FAST CUBE ROOT HELPER ---
// This is the canonical optimization. It replaces the expensive pow() call with
// a fast, cache-friendly lookup table with linear interpolation.
inline Halide::Expr fast_cube_root(Halide::Expr x, Halide::Func lut) {
    using namespace Halide;
    const int lut_size = 1024; // A 4KB LUT, fits easily in L1 cache.

    Expr float_idx = x * (lut_size - 1);
    Expr idx0 = cast<int>(float_idx);
    Expr idx1 = idx0 + 1;
    Expr frac = float_idx - idx0;

    // Clamp indices to be safe
    idx0 = clamp(idx0, 0, lut_size - 1);
    idx1 = clamp(idx1, 0, lut_size - 1);

    return lerp(lut(idx0), lut(idx1), frac);
}

// The non-linear function for converting XYZ to Lab.
inline Halide::Expr lab_f(Halide::Expr t, Halide::Func cube_root_lut) {
    using namespace Halide;
    const float t_thresh = 0.008856451679035631f; // (6/29)^3
    return select(t > t_thresh,
                  fast_cube_root(t, cube_root_lut), // Use the fast LUT-based version
                  (7.787037037037037f * t) + (16.0f / 116.0f));
}

// Converts CIE XYZ values to CIE Lab values, now requires the LUT.
inline Halide::Func xyz_to_lab(Halide::Func xyz, Halide::Var x, Halide::Var y, Halide::Var c, Halide::Func cube_root_lut, const std::string& name) {
    using namespace Halide;
    const float Xn = 0.95047f, Yn = 1.0f, Zn = 1.08883f;
    Expr X_ = xyz(x, y, 0)/Xn, Y_ = xyz(x, y, 1)/Yn, Z_ = xyz(x, y, 2)/Zn;
    Expr fX = lab_f(X_, cube_root_lut), fY = lab_f(Y_, cube_root_lut), fZ = lab_f(Z_, cube_root_lut);
    Expr L = 116.0f*fY - 16.0f, a = 500.0f*(fX - fY), b = 200.0f*(fY - fZ);
    Func lab(name); lab(x, y, c) = mux(c, {L, a, b}); return lab;
}

// --- Other helpers are unchanged ---
inline Halide::Func srgb_to_xyz(Halide::Func srgb_linear_01, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
    using namespace Halide;
    Expr r = srgb_linear_01(x, y, 0); Expr g = srgb_linear_01(x, y, 1); Expr b = srgb_linear_01(x, y, 2);
    Expr X = 0.4124564f*r + 0.3575761f*g + 0.1804375f*b, Y = 0.2126729f*r + 0.7151522f*g + 0.0721750f*b, Z = 0.0193339f*r + 0.1191920f*g + 0.9503041f*b;
    Func xyz(name); xyz(x, y, c) = mux(c, {X, Y, Z}); return xyz;
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
inline Halide::Func xyz_to_srgb(Halide::Func xyz, Halide::Var x, Halide::Var y, Halide::Var c, const std::string& name) {
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
    upx(x, y, _) = 0.25f * f((x/2) - 1 + 2*(x%2), y, _) + 0.75f * f(x/2, y, _);
    upy(x, y, _) = 0.25f * upx(x, (y/2) - 1 + 2*(y%2), _) + 0.75f * upx(x, y/2, _);
    output = upy;
    intermediates.push_back(upx);
}
} // anonymous namespace

template <typename T>
class LocalLaplacianBuilder_T {
public:
    // --- Halide Funcs exposed for scheduling ---
    Halide::Func output;
    std::vector<Halide::Func> gPyramid, lPyramid, inGPyramid, outGPyramid, outLPyramid;
    std::vector<Halide::Func> low_fi_intermediates, high_fi_intermediates, high_freq_pyramid_helpers, low_freq_pyramid_helpers, reconstruction_intermediates;
    Halide::Func remap_detail, cube_root_lut;
    const int pyramid_levels;
    Halide::Var qx, qy;

    // --- Constructor ---
    LocalLaplacianBuilder_T(Halide::Func input_rgb_high_fi,
                           Halide::Func raw_input,
                           Halide::Func cc_matrix,
                           Halide::Var x, Halide::Var y, Halide::Var c,
                           Halide::Expr detail_sharpen, Halide::Expr clarity,
                           Halide::Expr shadows, Halide::Expr highlights,
                           Halide::Expr blacks, Halide::Expr whites,
                           Halide::Expr blackLevel, Halide::Expr whiteLevel,
                           Halide::Expr width, Halide::Expr height,
                           int J = 8, int cutover_level = 4)
        : output("local_adjustments"),
          gPyramid(J), lPyramid(J), inGPyramid(J), outGPyramid(J), outLPyramid(J),
          pyramid_levels(J), qx("qx_lf"), qy("qy_lf")
    {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        // Define the cube root LUT.
        cube_root_lut = Func("cube_root_lut");
        Var i("i_lut");
        const int lut_size = 1024;
        Expr lut_val = pow(cast<float>(i) / (lut_size - 1.f), 1.f / 3.f);
        cube_root_lut(i) = lut_val;

        // --- PATH 1: High-Fidelity Path ---
        Func input_f_high_fi("laplacian_input_f_hifi"), clamped_input_hifi("clamped_input_hifi");
        Func xyz_hifi("xyz_hifi"), lab_hifi("lab_hifi"), L_norm_hifi("L_norm_hifi");
        Func a_chan("a_chan"), b_chan("b_chan");
        if (std::is_same<T, uint16_t>::value) {
            input_f_high_fi(x, y, c) = cast<float>(input_rgb_high_fi(x, y, c)) / 65535.0f;
        } else { input_f_high_fi(x, y, c) = input_rgb_high_fi(x, y, c); }
        clamped_input_hifi = BoundaryConditions::repeat_edge(input_f_high_fi, {{0, width}, {0, height}, {0, 3}});
        xyz_hifi = srgb_to_xyz(clamped_input_hifi, x, y, c, "xyz_hifi");
        lab_hifi = xyz_to_lab(xyz_hifi, x, y, c, cube_root_lut, "lab_hifi");
        L_norm_hifi(x, y) = lab_hifi(x, y, 0) / 100.0f;
        a_chan(x, y) = lab_hifi(x, y, 1); b_chan(x, y) = lab_hifi(x, y, 2);
        high_fi_intermediates = {input_f_high_fi, clamped_input_hifi, xyz_hifi, lab_hifi, L_norm_hifi, a_chan, b_chan};
        
        // --- PATH 2: Low-Fidelity Path ---
        Func L_norm_lowfi("L_norm_lowfi");
        {
            Func deinterleaved_raw = pipeline_deinterleave(raw_input, qx, qy, c);
            Func rgb_lowfi("rgb_lowfi");
            rgb_lowfi(qx, qy, c) = mux(c, {cast<float>(deinterleaved_raw(qx, qy, 1)), avg(cast<float>(deinterleaved_raw(qx, qy, 0)), cast<float>(deinterleaved_raw(qx, qy, 3))), cast<float>(deinterleaved_raw(qx, qy, 2))});
            Func corrected_lowfi("corrected_lowfi");
            Expr ir = rgb_lowfi(qx, qy, 0), ig = rgb_lowfi(qx, qy, 1), ib = rgb_lowfi(qx, qy, 2);
            corrected_lowfi(qx, qy, c) = mux(c, {cc_matrix(3,0) + cc_matrix(0,0)*ir + cc_matrix(1,0)*ig + cc_matrix(2,0)*ib, cc_matrix(3,1) + cc_matrix(0,1)*ir + cc_matrix(1,1)*ig + cc_matrix(2,1)*ib, cc_matrix(3,2) + cc_matrix(0,2)*ir + cc_matrix(1,2)*ig + cc_matrix(2,2)*ib});
            Func input_f_lowfi("input_f_lowfi");
            Expr inv_range = 1.0f / (cast<float>(whiteLevel) - blackLevel);
            input_f_lowfi(qx, qy, c) = (corrected_lowfi(qx, qy, c) - blackLevel) * inv_range;
            Func clamped_input_lowfi = BoundaryConditions::repeat_edge(input_f_lowfi, {{0, width/2}, {0, height/2}, {0, 3}});
            Func xyz_lowfi = srgb_to_xyz(clamped_input_lowfi, qx, qy, c, "xyz_lowfi");
            Func lab_lowfi = xyz_to_lab(xyz_lowfi, qx, qy, c, cube_root_lut, "lab_lowfi");
            L_norm_lowfi(qx, qy) = lab_lowfi(qx, qy, 0) / 100.0f;
            low_fi_intermediates = {deinterleaved_raw, rgb_lowfi, corrected_lowfi, input_f_lowfi, clamped_input_lowfi, xyz_lowfi, lab_lowfi, L_norm_lowfi};
        }

        // --- Build Forked Pyramids ---
        remap_detail = Func("remap_detail_lut");
        remap_detail(x) = (detail_sharpen/100.f) * (cast<float>(x)/256.f) * Halide::exp(-(cast<float>(x)/256.f)*(cast<float>(x)/256.f) / 2.0f);
        Halide::Var k("k_laplacian");
        Expr level_k = cast<float>(k) / (pyramid_levels - 1);
        Expr remapped_level = level_k + (shadows/100.f)*(1.f-smoothstep(0.f,0.5f,level_k)) + (highlights/100.f)*smoothstep(0.5f,1.f,level_k);
        gPyramid[0] = Func("gPyramid_0");
        gPyramid[0](x, y, k) = (1.f+clarity/100.f) * (L_norm_hifi(x,y) - level_k) + remapped_level + remap_detail(cast<int>(clamp(L_norm_hifi(x,y)*256.f, 0.f, 255.f)));
        inGPyramid[0] = L_norm_hifi;
        for (int j = 1; j < cutover_level; j++) {
            Func clamped_g = BoundaryConditions::repeat_edge(gPyramid[j-1], {{0, width/(1<<(j-1))}, {0, height/(1<<(j-1))}, {0, J}});
            downsample(clamped_g, "gpyr_ds_"+std::to_string(j), gPyramid[j], high_freq_pyramid_helpers);
            Func clamped_in = BoundaryConditions::repeat_edge(inGPyramid[j-1], {{0, width/(1<<(j-1))}, {0, height/(1<<(j-1))}});
            downsample(clamped_in, "inGpyr_ds_"+std::to_string(j), inGPyramid[j], high_freq_pyramid_helpers);
            high_freq_pyramid_helpers.push_back(clamped_g); high_freq_pyramid_helpers.push_back(clamped_in);
        }
        gPyramid[cutover_level] = Func("gPyramid_" + std::to_string(cutover_level));
        gPyramid[cutover_level](qx, qy, k) = (1.f+clarity/100.f) * (L_norm_lowfi(qx,qy) - level_k) + remapped_level + remap_detail(cast<int>(clamp(L_norm_lowfi(qx,qy)*256.f, 0.f, 255.f)));
        inGPyramid[cutover_level] = L_norm_lowfi;
        for (int j = cutover_level + 1; j < pyramid_levels; j++) {
            Func clamped_g = BoundaryConditions::repeat_edge(gPyramid[j-1], {{0, width/(1<<(j-1))}, {0, height/(1<<(j-1))}, {0, J}});
            downsample(clamped_g, "gpyr_ds_"+std::to_string(j), gPyramid[j], low_freq_pyramid_helpers);
            Func clamped_in = BoundaryConditions::repeat_edge(inGPyramid[j-1], {{0, width/(1<<(j-1))}, {0, height/(1<<(j-1))}});
            downsample(clamped_in, "inGpyr_ds_"+std::to_string(j), inGPyramid[j], low_freq_pyramid_helpers);
            low_freq_pyramid_helpers.push_back(clamped_g); low_freq_pyramid_helpers.push_back(clamped_in);
        }

        // --- Reconstruct Final Image ---
        lPyramid[pyramid_levels-1] = gPyramid[pyramid_levels-1];
        for(int j=pyramid_levels-2; j>=0; j--) {
            lPyramid[j] = Func("lPyramid_"+std::to_string(j));
            Func upsampled_g;
            auto& helpers = (j >= cutover_level) ? low_freq_pyramid_helpers : high_freq_pyramid_helpers;
            Func clamped_g = BoundaryConditions::repeat_edge(gPyramid[j+1], {{0, width/(1<<(j+1))}, {0, height/(1<<(j+1))}, {0, J}});
            upsample(clamped_g, "lpyr_us_"+std::to_string(j), upsampled_g, helpers);
            lPyramid[j](x,y,k) = gPyramid[j](x,y,k) - upsampled_g(x,y,k);
            helpers.push_back(clamped_g);
        }
        for(int j=0; j<pyramid_levels; j++) {
            outLPyramid[j] = Func("outLPyramid_"+std::to_string(j));
            Expr level_val = inGPyramid[j](x,y) * (pyramid_levels-1);
            Expr li = clamp(cast<int>(level_val), 0, pyramid_levels-2);
            outLPyramid[j](x,y) = lerp(lPyramid[j](x,y,li), lPyramid[j](x,y,li+1), level_val - cast<float>(li));
        }
        outGPyramid[pyramid_levels-1] = outLPyramid[pyramid_levels-1];
        for(int j=pyramid_levels-2; j>=0; j--) {
            outGPyramid[j] = Func("outGPyramid_"+std::to_string(j));
            Func upsampled_out;
            auto& helpers = (j >= cutover_level) ? low_freq_pyramid_helpers : high_freq_pyramid_helpers;
            Func clamped_out = BoundaryConditions::repeat_edge(outGPyramid[j+1], {{0, width/(1<<(j+1))}, {0, height/(1<<(j+1))}});
            upsample(clamped_out, "outGpyr_us_"+std::to_string(j), upsampled_out, helpers);
            outGPyramid[j](x,y) = upsampled_out(x,y) + outLPyramid[j](x,y);
            helpers.push_back(clamped_out);
        }
        Func L_out("L_out"), lab_out("lab_out"), xyz_out("xyz_out"), rgb_out_f("rgb_out_f"), final_adjustments("final_adjustments");
        L_out(x, y) = outGPyramid[0](x, y) * 100.0f;
        lab_out(x, y, c) = mux(c, {L_out(x, y), a_chan(x, y), b_chan(x, y)});
        xyz_out = lab_to_xyz(lab_out, x, y, c, "final_xyz");
        rgb_out_f = xyz_to_srgb(xyz_out, x, y, c, "final_rgb_f");
        Expr blacks_level = blacks/250.f, whites_level = 1.f + whites/250.f;
        Expr remap_denom = whites_level - blacks_level;
        final_adjustments(x,y,c) = (rgb_out_f(x,y,c) - blacks_level) / select(abs(remap_denom)<1e-5f, 1e-5f, remap_denom);
        reconstruction_intermediates = {L_out, lab_out, xyz_out, rgb_out_f, final_adjustments};
        Expr is_default = detail_sharpen == 0 && clarity == 0 && shadows == 0 && highlights == 0 && blacks == 0 && whites == 0;
        Expr final_val = select(is_default, input_f_high_fi(x, y, c), final_adjustments(x, y, c));
        if (std::is_same<T, uint16_t>::value) {
            output(x, y, c) = proc_type_sat<T>(final_val * 65535.0f);
        } else {
            output(x, y, c) = proc_type_sat<T>(final_val);
        }
    }
};

#endif // STAGE_LOCAL_ADJUST_LAPLACIAN_H
