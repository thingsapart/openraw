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
class CACorrectBuilder {
public:
    Halide::Func output;
    // Empty members for API compatibility with the scheduled pipeline.
    Halide::Func g_interp, block_shifts, blur_x, blur_y;
    Halide::Var bx, by;

    CACorrectBuilder(Halide::Func input_float,
                     Halide::Var x, Halide::Var y,
                     Halide::Expr strength,
                     Halide::Expr width, Halide::Expr height,
                     const Halide::Target &target,
                     bool is_autoscheduled) {
        output = Halide::Func("ca_corrected_dummy");
        output(x, y) = input_float(x, y);
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


class CACorrectBuilder {
public:
    Halide::Func output;
    // Expose key internal funcs for scheduling
    Halide::Func g_interp;
    Halide::Func block_shifts;
    Halide::Func blur_x;
    Halide::Func blur_y;
    // Expose the vars used for the coarse grid for scheduling
    Halide::Var bx, by;


    CACorrectBuilder(Halide::Func input_float,
                     Halide::Var x, Halide::Var y,
                     Halide::Expr strength,
                     Halide::Expr width, Halide::Expr height,
                     const Halide::Target &target,
                     bool is_autoscheduled) :
        output("ca_corrected"),
        g_interp("ca_g_interp"),
        block_shifts("ca_block_shifts"),
        blur_x("ca_blur_x"),
        blur_y("ca_blur_y"),
        bx("ca_bx"), by("ca_by")
    {
        using namespace Halide;
        using namespace Halide::ConciseCasts;
        
        // The input 'norm_raw' is already a normalized [0,1] float.
        Func norm_raw = input_float;

        // The tile size for the coarse-grid shift estimation.
        const int ts = 16;

        // 2. Determine color of each pixel from GRBG Bayer pattern
        Expr cfa_y = y % 2;
        Expr cfa_x = x % 2;
        Expr is_g = (cfa_y + cfa_x) % 2 == 0;
        Expr is_r = (cfa_y == 0 && cfa_x == 1);
        Expr is_b = (cfa_y == 1 && cfa_x == 0);

        // 3. Interpolate Green channel to R and B sites.
        {
            Func clamped_norm = BoundaryConditions::repeat_edge(norm_raw, {{Expr(0), width}, {Expr(0), height}});
            Expr g_n = clamped_norm(x, y - 1);
            Expr g_s = clamped_norm(x, y + 1);
            Expr g_w = clamped_norm(x - 1, y);
            Expr g_e = clamped_norm(x + 1, y);

            Expr grad_v = absd(g_n, g_s);
            Expr grad_h = absd(g_w, g_e);

            // Directionally-adaptive interpolation based on absolute difference.
            Expr interp_val = select(grad_h < grad_v, avg(g_w, g_e), avg(g_n, g_s));

            g_interp(x, y) = select(is_g, norm_raw(x, y), interp_val);
        }

        // 4. Estimate shifts on a coarse grid.
        Var c_ca("c_ca"), v_ca("v_ca");
        {
            RDom r(0, ts, 0, ts, "ca_rdom");
            Expr tile_x = bx * ts + r.x;
            Expr tile_y = by * ts + r.y;

            Func clamped_g_interp = BoundaryConditions::repeat_edge(g_interp, {{0, width}, {0, height}});
            Func clamped_norm_raw = BoundaryConditions::repeat_edge(norm_raw, {{0, width}, {0, height}});

            Expr is_tile_r = ((tile_y % 2) == 0 && (tile_x % 2) == 1);
            Expr is_tile_b = ((tile_y % 2) == 1 && (tile_x % 2) == 0);

            Expr C = clamped_norm_raw(tile_x, tile_y);
            Expr G = clamped_g_interp(tile_x, tile_y);
            Expr deltgrb = G - C;
            
            // This threshold prevents division by zero in flat areas (shadows, highlights)
            // which is the likely cause of the visual artifacts.
            const float den_thresh = 0.001f; 

            // --- Horizontal shifts (v=1) ---
            Expr gdiff_h = clamped_g_interp(tile_x + 1, tile_y) - clamped_g_interp(tile_x - 1, tile_y);
            Expr num_h_r = sum(select(is_tile_r, deltgrb * gdiff_h, 0.f), "ca_num_h_r_sum");
            Expr den_h_r = sum(select(is_tile_r, gdiff_h * gdiff_h, 0.f), "ca_den_h_r_sum");
            Expr shift_h_r = num_h_r / (den_h_r + 1e-5f);

            Expr num_h_b = sum(select(is_tile_b, deltgrb * gdiff_h, 0.f), "ca_num_h_b_sum");
            Expr den_h_b = sum(select(is_tile_b, gdiff_h * gdiff_h, 0.f), "ca_den_h_b_sum");
            Expr shift_h_b = num_h_b / (den_h_b + 1e-5f);

            // --- Vertical shifts (v=0) ---
            Expr gdiff_v = clamped_g_interp(tile_x, tile_y + 1) - clamped_g_interp(tile_x, tile_y - 1);
            Expr num_v_r = sum(select(is_tile_r, deltgrb * gdiff_v, 0.f), "ca_num_v_r_sum");
            Expr den_v_r = sum(select(is_tile_r, gdiff_v * gdiff_v, 0.f), "ca_den_v_r_sum");
            Expr shift_v_r = num_v_r / (den_v_r + 1e-5f);

            Expr num_v_b = sum(select(is_tile_b, deltgrb * gdiff_v, 0.f), "ca_num_v_b_sum");
            Expr den_v_b = sum(select(is_tile_b, gdiff_v * gdiff_v, 0.f), "ca_den_v_b_sum");
            Expr shift_v_b = num_v_b / (den_v_b + 1e-5f);
            
            const float bslim = 3.99f;
            // c=0 is R, c=1 is B
            // v=0 is vert, v=1 is horiz
            Expr shift_v = select(c_ca == 0, shift_v_r, shift_v_b);
            Expr shift_h = select(c_ca == 0, shift_h_r, shift_h_b);
            Expr shift = select(v_ca == 0, shift_v, shift_h);

            block_shifts(bx, by, c_ca, v_ca) = clamp(shift, -bslim, bslim);
            block_shifts.set_estimates({ {0, (width + ts - 1) / ts}, {0, (height + ts - 1) / ts}, {0, 2}, {0, 2} });
        }

        // 5. Blur the block_shifts to get a smooth global shift field.
        Func blurred_shifts("blurred_shifts");
        {
            Region block_bounds = {{0, (width+ts-1)/ts}, {0, (height+ts-1)/ts}, {0,2}, {0,2}};
            Func clamped_shifts = BoundaryConditions::repeat_edge(block_shifts, block_bounds);
            
            RDom r_blur(-4, 9, "ca_blur_rdom");
            blur_x(bx, by, c_ca, v_ca) = sum(clamped_shifts(bx + r_blur, by, c_ca, v_ca), "ca_shifts_blur_x_sum");
            blur_y(bx, by, c_ca, v_ca) = sum(blur_x(bx, by + r_blur, c_ca, v_ca), "ca_shifts_blur_y_sum");
            blurred_shifts(bx, by, c_ca, v_ca) = blur_y(bx, by, c_ca, v_ca) / 81.0f;
        }

        // 6. Apply the correction to R and B channels
        Func corrected_f("corrected_f");
        {
            Expr f_bx = cast<float>(x) / (float)ts;
            Expr f_by = cast<float>(y) / (float)ts;

            const float shift_bound = 4.0f;
            Expr shift_vr = clamp(bilinear(blurred_shifts, f_bx, f_by, 0, 0), -shift_bound, shift_bound) * strength;
            Expr shift_hr = clamp(bilinear(blurred_shifts, f_bx, f_by, 0, 1), -shift_bound, shift_bound) * strength;
            Expr shift_vb = clamp(bilinear(blurred_shifts, f_bx, f_by, 1, 0), -shift_bound, shift_bound) * strength;
            Expr shift_hb = clamp(bilinear(blurred_shifts, f_bx, f_by, 1, 1), -shift_bound, shift_bound) * strength;
            
            Func g_interp_clamped = BoundaryConditions::repeat_edge(g_interp, {{0, width}, {0, height}});
            Expr r_new = norm_raw(x, y) + g_interp(x, y) - bilinear(g_interp_clamped, x + shift_hr, y + shift_vr);
            Expr b_new = norm_raw(x, y) + g_interp(x, y) - bilinear(g_interp_clamped, x + shift_hb, y + shift_vb);
            
            corrected_f(x, y) = select(is_r, r_new, is_b, b_new, norm_raw(x, y));
        }

        // This stage is a no-op if strength is zero.
        output(x, y) = select(strength < 0.001f,
                              input_float(x, y),
                              clamp(corrected_f(x,y), 0.0f, 1.0f));

        // The pointwise helpers 'blurred_shifts' and 'corrected_f' will be inlined
        // by default because they are not scheduled.
    }
};

#endif // NO_CA_CORRECT

#endif // STAGE_CA_CORRECT_H
