#ifndef STAGE_CA_CORRECT_H
#define STAGE_CA_CORRECT_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include <vector>
#include <type_traits>

/*
    This stage implements a chromatic aberration correction algorithm.
    The implementation is a simplified version of the algorithm found in darktable,
    which itself is based on RawTherapee code.
*/

#ifdef NO_CA_CORRECT
template <typename T>
class CACorrectBuilder_T {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;

    CACorrectBuilder_T(Halide::Func input_raw,
                     Halide::Var x, Halide::Var y,
                     Halide::Expr strength,
                     Halide::Expr blackLevel, Halide::Expr whiteLevel,
                     Halide::Expr width, Halide::Expr height,
                     const Halide::Target &target,
                     bool is_autoscheduled) {
        output = Halide::Func("ca_corrected_dummy");
        output(x, y) = input_raw(x, y);
    }
};
#else

namespace { // Anonymous namespace for helpers

// Bilinear interpolation for a 2D Func
inline Halide::Expr bilinear(Halide::Func f, Halide::Expr x, Halide::Expr y) {
    using namespace Halide;
    Expr xf = x - floor(x);
    Expr yf = y - floor(y);
    Expr xi = cast<int>(floor(x));
    Expr yi = cast<int>(floor(y));

    Expr v00 = f(xi, yi);
    Expr v10 = f(xi + 1, yi);
    Expr v01 = f(xi, yi + 1);
    Expr v11 = f(xi + 1, yi + 1);

    Expr v0 = lerp(v00, v10, xf);
    Expr v1 = lerp(v01, v11, xf);

    return lerp(v0, v1, yf);
}

// Bilinear interpolation for a 4D Func on its first two dimensions
inline Halide::Expr bilinear(Halide::Func f, Halide::Expr x, Halide::Expr y, Halide::Expr c0, Halide::Expr c1) {
    using namespace Halide;
    Expr xf = x - floor(x);
    Expr yf = y - floor(y);
    Expr xi = cast<int>(floor(x));
    Expr yi = cast<int>(floor(y));

    Expr v00 = f(xi, yi, c0, c1);
    Expr v10 = f(xi + 1, yi, c0, c1);
    Expr v01 = f(xi, yi + 1, c0, c1);
    Expr v11 = f(xi + 1, yi + 1, c0, c1);

    Expr v0 = lerp(v00, v10, xf);
    Expr v1 = lerp(v01, v11, xf);

    return lerp(v0, v1, yf);
}

} // namespace


template <typename T>
class CACorrectBuilder_T {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;

    CACorrectBuilder_T(Halide::Func input_raw,
                     Halide::Var x, Halide::Var y,
                     Halide::Expr strength,
                     Halide::Expr blackLevel, Halide::Expr whiteLevel,
                     Halide::Expr width, Halide::Expr height,
                     const Halide::Target &target,
                     bool is_autoscheduled) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        output = Halide::Func("ca_corrected");
        Halide::Type proc_type = input_raw.type();

        // --- BOUNDARY CONDITION ---
        Func clamped_input("ca_clamped_input");
        clamped_input = BoundaryConditions::repeat_edge(input_raw, {{0, width}, {0, height}});

        // 1. Get a normalized float representation of the input.
        Func input_f("ca_input_f");
        if (std::is_same<T, float>::value) {
            input_f(x, y) = clamped_input(x, y);
        } else {
            Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
            input_f(x, y) = (cast<float>(clamped_input(x, y)) - blackLevel) * inv_range;
        }

        // 2. Determine color of each pixel from GRBG Bayer pattern
        Expr cfa_y = y % 2;
        Expr cfa_x = x % 2;
        Expr is_g = (cfa_y + cfa_x) % 2 == 0;
        Expr is_r = (cfa_y == 0 && cfa_x == 1);
        Expr is_b = (cfa_y == 1 && cfa_x == 0);

        // 3. Interpolate Green channel to R and B sites.
        Func g_interp("g_interp");
        {
            Expr g_n = input_f(x, y - 1); Expr g_s = input_f(x, y + 1);
            Expr g_w = input_f(x - 1, y); Expr g_e = input_f(x + 1, y);
            Expr grad_v = absd(g_n, g_s); Expr grad_h = absd(g_w, g_e);
            Expr weight_v = 1.0f / (1e-5f + grad_v); Expr weight_h = 1.0f / (1e-5f + grad_h);
            Expr interp_val = (g_n + g_s) * weight_v + (g_w + g_e) * weight_h;
            interp_val = interp_val / (2.f * weight_v + 2.f * weight_h);
            g_interp(x, y) = select(is_g, input_f(x, y), interp_val);
        }

        // 4. Estimate shifts on a coarse grid.
        Func block_shifts("block_shifts");
        Var bx("bx"), by("by"), c("c_ca"), v("v_ca");
        {
            int ts = 32; RDom r(0, ts, 0, ts, "ca_rdom");
            Expr tx = bx * ts + r.x, ty = by * ts + r.y;
            Expr is_tr = ((ty % 2) == 0 && (tx % 2) == 1), is_tb = ((ty % 2) == 1 && (tx % 2) == 0);
            Expr C = input_f(tx, ty), G = g_interp(tx, ty), delt = G - C;
            Expr gdh = g_interp(tx + 1, ty) - g_interp(tx - 1, ty);
            Expr den_hr = sum(select(is_tr, gdh*gdh, 0.f)), num_hr = sum(select(is_tr, delt*gdh, 0.f));
            Expr den_hb = sum(select(is_tb, gdh*gdh, 0.f)), num_hb = sum(select(is_tb, delt*gdh, 0.f));
            Expr gdv = g_interp(tx, ty + 1) - g_interp(tx, ty - 1);
            Expr den_vr = sum(select(is_tr, gdv*gdv, 0.f)), num_vr = sum(select(is_tr, delt*gdv, 0.f));
            Expr den_vb = sum(select(is_tb, gdv*gdv, 0.f)), num_vb = sum(select(is_tb, delt*gdv, 0.f));
            Expr sh_r = num_hr/(den_hr+1e-5f), sh_b = num_hb/(den_hb+1e-5f);
            Expr sv_r = num_vr/(den_vr+1e-5f), sv_b = num_vb/(den_vb+1e-5f);
            Expr sh = select(c==0, sh_r, sh_b), sv = select(c==0, sv_r, sv_b);
            block_shifts(bx,by,c,v) = clamp(select(v==0, sv, sh), -3.99f, 3.99f);
        }

        // 5. Blur the block_shifts to get a smooth global shift field.
        Func blurred_shifts("blurred_shifts");
        {
            // Because block_shifts has a complex definition (with a sum),
            // Halide cannot infer its bounds. We must provide them explicitly.
            const int ts = 32;
            Region block_bounds = {{0, (width + ts - 1) / ts},
                                   {0, (height + ts - 1) / ts},
                                   {0, 2},
                                   {0, 2}};
            Func clamped_shifts = BoundaryConditions::repeat_edge(block_shifts, block_bounds);
            Func blur_x("blur_x_shifts"); RDom r_b(-4,9);
            blur_x(bx, by, c, v) = sum(clamped_shifts(bx + r_b, by, c, v));
            blurred_shifts(bx, by, c, v) = sum(blur_x(bx, by + r_b, c, v)) / 81.f;
            intermediates.push_back(blur_x); intermediates.push_back(clamped_shifts);
        }

        // 6. Apply the correction. The result is `corrected_f`, a float Func.
        Func corrected_f("corrected_f");
        {
            Expr fbx = cast<float>(x)/32.f, fby=cast<float>(y)/32.f;
            Expr svr = clamp(bilinear(blurred_shifts,fbx,fby,0,0),-4.f,4.f)*strength;
            Expr shr = clamp(bilinear(blurred_shifts,fbx,fby,0,1),-4.f,4.f)*strength;
            Expr svb = clamp(bilinear(blurred_shifts,fbx,fby,1,0),-4.f,4.f)*strength;
            Expr shb = clamp(bilinear(blurred_shifts,fbx,fby,1,1),-4.f,4.f)*strength;
            Expr r_new = input_f(x,y)+g_interp(x,y)-bilinear(g_interp,x+shr,y+svr);
            Expr b_new = input_f(x,y)+g_interp(x,y)-bilinear(g_interp,x+shb,y+svb);
            corrected_f(x,y) = select(is_r,r_new,is_b,b_new,input_f(x,y));
        }

        // 7. Produce the final output, correctly typed.
        if (std::is_same<T, float>::value) {
            output(x, y) = select(strength < 0.001f, input_raw(x, y), corrected_f(x, y));
        } else {
            Func denormalized("denormalized");
            Expr denorm_val = corrected_f(x, y) * (whiteLevel - blackLevel) + blackLevel;
            denormalized(x, y) = cast(proc_type, proc_type_sat<T>(denorm_val));
            intermediates.push_back(denormalized);
            output(x, y) = select(strength < 0.001f, input_raw(x, y), denormalized(x, y));
        }

        intermediates.insert(intermediates.end(), {clamped_input, input_f, g_interp, block_shifts, blurred_shifts, corrected_f});
    }
};

#endif // NO_CA_CORRECT

#endif // STAGE_CA_CORRECT_H
