#ifndef STAGE_CA_CORRECT_H
#define STAGE_CA_CORRECT_H

#include "Halide.h"
#include <vector>

/*
    This stage implements a chromatic aberration correction algorithm.
    The implementation is a simplified version of the algorithm found in darktable,
    which itself is based on RawTherapee code.

    Original authors:
    Copyright (c) 2008-2010  Emil Martinec <ejmartin@uchicago.edu>
    Copyright (c) for improvements 2018 Ingo Weyrich <heckflosse67@gmx.de>
    This Halide implementation by the user of this model.

    The original algorithm uses a global polynomial fit to model CA, which is
    difficult to implement efficiently in Halide. This version approximates that
    step by calculating shifts on a coarse grid and then blurring them to get
    a smooth, global shift field.
*/

#ifdef NO_CA_CORRECT
class CACorrectBuilder {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;

    CACorrectBuilder(Halide::Func input_raw,
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


class CACorrectBuilder {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;

    CACorrectBuilder(Halide::Func input_raw,
                     Halide::Var x, Halide::Var y,
                     Halide::Expr strength,
                     Halide::Expr blackLevel, Halide::Expr whiteLevel,
                     Halide::Expr width, Halide::Expr height,
                     const Halide::Target &target,
                     bool is_autoscheduled) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        output = Halide::Func("ca_corrected");

        // 1. Normalize input to float [0, 1]
        Func norm_raw("norm_raw");
        norm_raw(x, y) = (cast<float>(input_raw(x, y)) - blackLevel) / (whiteLevel - blackLevel);

        // 2. Determine color of each pixel from GRBG Bayer pattern
        Expr cfa_y = y % 2;
        Expr cfa_x = x % 2;
        Expr is_g = (cfa_y + cfa_x) % 2 == 0;
        Expr is_r = (cfa_y == 0 && cfa_x == 1);
        Expr is_b = (cfa_y == 1 && cfa_x == 0);

        // 3. Interpolate Green channel to R and B sites.
        Func g_interp("g_interp");
        {
            Func clamped_norm = BoundaryConditions::repeat_edge(norm_raw, {{Expr(0), width}, {Expr(0), height}});
            Expr g_n = clamped_norm(x, y - 1);
            Expr g_s = clamped_norm(x, y + 1);
            Expr g_w = clamped_norm(x - 1, y);
            Expr g_e = clamped_norm(x + 1, y);

            Expr grad_v = absd(g_n, g_s);
            Expr grad_h = absd(g_w, g_e);
            Expr weight_v = 1.0f / (1e-5f + grad_v);
            Expr weight_h = 1.0f / (1e-5f + grad_h);

            Expr interp_val = (g_n + g_s) * weight_v + (g_w + g_e) * weight_h;
            interp_val = interp_val / (2.f * weight_v + 2.f * weight_h);
            g_interp(x, y) = select(is_g, norm_raw(x, y), interp_val);
        }

        // 4. Estimate shifts on a coarse grid.
        Func block_shifts("block_shifts");
        Var bx("bx"), by("by"), c("c_ca"), v("v_ca");
        {
            int ts = 32;
            RDom r(0, ts, 0, ts);
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
            Expr num_h_r = sum(select(is_tile_r, deltgrb * gdiff_h, 0.f));
            Expr den_h_r = sum(select(is_tile_r, gdiff_h * gdiff_h, 0.f));
            Expr shift_h_r = select(den_h_r > den_thresh, num_h_r / den_h_r, 0.0f);

            Expr num_h_b = sum(select(is_tile_b, deltgrb * gdiff_h, 0.f));
            Expr den_h_b = sum(select(is_tile_b, gdiff_h * gdiff_h, 0.f));
            Expr shift_h_b = select(den_h_b > den_thresh, num_h_b / den_h_b, 0.0f);

            // --- Vertical shifts (v=0) ---
            Expr gdiff_v = clamped_g_interp(tile_x, tile_y + 1) - clamped_g_interp(tile_x, tile_y - 1);
            Expr num_v_r = sum(select(is_tile_r, deltgrb * gdiff_v, 0.f));
            Expr den_v_r = sum(select(is_tile_r, gdiff_v * gdiff_v, 0.f));
            Expr shift_v_r = select(den_v_r > den_thresh, num_v_r / den_v_r, 0.0f);

            Expr num_v_b = sum(select(is_tile_b, deltgrb * gdiff_v, 0.f));
            Expr den_v_b = sum(select(is_tile_b, gdiff_v * gdiff_v, 0.f));
            Expr shift_v_b = select(den_v_b > den_thresh, num_v_b / den_v_b, 0.0f);
            
            const float bslim = 3.99f;
            // c=0 is R, c=1 is B
            // v=0 is vert, v=1 is horiz
            Expr shift_v = select(c == 0, shift_v_r, shift_v_b);
            Expr shift_h = select(c == 0, shift_h_r, shift_h_b);
            Expr shift = select(v == 0, shift_v, shift_h);

            block_shifts(bx, by, c, v) = clamp(shift, -bslim, bslim);

            // Add estimates to help the autoscheduler determine the buffer size.
            // This MUST be done after the function is defined.
            Expr bx_extent = (width + ts - 1) / ts;
            Expr by_extent = (height + ts - 1) / ts;
            block_shifts.set_estimates({ {0, bx_extent}, {0, by_extent}, {0, 2}, {0, 2} });
        }

        // 5. Blur the block_shifts to get a smooth global shift field.
        Func blurred_shifts("blurred_shifts");
        {
            int ts = 32;
            Region block_bounds = {{0, (width+ts-1)/ts}, {0, (height+ts-1)/ts}, {0,2}, {0,2}};
            Func clamped_shifts = BoundaryConditions::repeat_edge(block_shifts, block_bounds);
            
            Func blur_x("blur_x_shifts"), blur_y("blur_y_shifts");
            RDom r_blur(-4, 9);
            blur_x(bx, by, c, v) = sum(clamped_shifts(bx + r_blur, by, c, v));
            blur_y(bx, by, c, v) = sum(blur_x(bx, by + r_blur, c, v));
            blurred_shifts(bx, by, c, v) = blur_y(bx, by, c, v) / 81.0f;
            intermediates.push_back(blur_x);
            intermediates.push_back(blur_y);
        }

        // 6. Apply the correction to R and B channels
        Func corrected_f("corrected_f");
        {
            Expr f_bx = cast<float>(x) / 32.f;
            Expr f_by = cast<float>(y) / 32.f;

            // The auto-scheduler needs to know the bounds of the resampling offsets.
            // Although the shifts are clamped earlier, we clamp them again here
            // to make the bounds explicit for the static analysis.
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

        // 7. Denormalize and convert back to uint16
        Func denormalized("denormalized");
        denormalized(x, y) = corrected_f(x, y) * (whiteLevel - blackLevel) + blackLevel;

        // This stage is a no-op if strength is zero.
        output(x, y) = select(strength < 0.001f,
                              input_raw(x, y),
                              cast<uint16_t>(clamp(denormalized(x, y), 0, 65535)));

        intermediates.push_back(norm_raw);
        intermediates.push_back(g_interp);
        intermediates.push_back(block_shifts);
        intermediates.push_back(blurred_shifts);
        intermediates.push_back(corrected_f);
        intermediates.push_back(denormalized);
    }
};

#endif // NO_CA_CORRECT

#endif // STAGE_CA_CORRECT_H
