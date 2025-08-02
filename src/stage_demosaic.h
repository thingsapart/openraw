#ifndef STAGE_DEMOSAIC_H
#define STAGE_DEMOSAIC_H

#include "Halide.h"
#include <vector>
#include <string>
#include <algorithm>

// DemosaicBuilder is a C++ class that constructs the Halide algorithm for the demosaic stage.
// It now operates entirely on Float(32) data to prevent integer underflow/overflow artifacts.
// It can be constructed with a choice of algorithm.
class DemosaicBuilder {
private:
    // All internal helpers now operate on float Exprs.
    using Expr = Halide::Expr;
    using Func = Halide::Func;
    using Var = Halide::Var;
    using Region = Halide::Region;
    using RDom = Halide::RDom;

    Expr width_expr, height_expr;

    // Helper to interleave four quarter-resolution Funcs into a single full-resolution one.
    Func interleave(Func gr, Func r, Func b, Func gb) {
        Var x("ix"), y("iy");
        Func out("interleaved_demosaic");
        Expr is_y_even = (y % 2 == 0);
        Expr is_x_even = (x % 2 == 0);
        Expr half_x = x / 2;
        Expr half_y = y / 2;
        out(x, y) = select(is_y_even,
                           select(is_x_even, gr(half_x, half_y), r(half_x, half_y)),
                           select(is_x_even, b(half_x, half_y), gb(half_x, half_y)));
        return out;
    }

    // Each build function now takes the Func it's supposed to define as its first argument.
    void build_simple(Func result, Func deinterleaved, Var x, Var y, Var c);
    void build_vhg(Func result, Func deinterleaved, Var x, Var y, Var c);
    void build_ahd(Func result, Func deinterleaved, Var x, Var y, Var c);
    void build_lmmse(Func result, Func deinterleaved, Var x, Var y, Var c);

public:
    Func output;
    // Full-resolution algorithm outputs, exposed for individual scheduling.
    Func simple_output, vhg_output, ahd_output, lmmse_output;
    // All intermediate Funcs that need to be scheduled by the parent.
    std::vector<Func> quarter_res_intermediates;
    std::vector<Func> full_res_intermediates;
    // The Vars used to define the quarter-sized intermediates, exposed for scheduling.
    Var qx, qy;

    DemosaicBuilder(Func deinterleaved, Var x, Var y, Var c, Expr algorithm, Expr width, Expr height)
        : qx("d_qx"), qy("d_qy"), width_expr(width), height_expr(height)
    {
        // Clamp the input deinterleaved Func to handle boundary conditions robustly.
        Region bounds = {{Expr(0), width / 2}, {Expr(0), height / 2}, {Expr(0), 4}};
        Func deinterleaved_clamped = Halide::BoundaryConditions::repeat_edge(deinterleaved, bounds);
        
        // --- Define all algorithm paths ---
        simple_output = Func("demosaiced_simple");
        vhg_output = Func("demosaiced_vhg");
        ahd_output = Func("demosaiced_ahd");
        lmmse_output = Func("demosaiced_lmmse");

        // Build each path by passing the target Func to the helper.
        build_simple(simple_output, deinterleaved_clamped, x, y, c);
        build_vhg(vhg_output, deinterleaved_clamped, x, y, c);
        build_ahd(ahd_output, deinterleaved_clamped, x, y, c);
        build_lmmse(lmmse_output, deinterleaved_clamped, x, y, c);

        // --- Final Selection ---
        // Use a nested select to choose the final algorithm.
        output = Func("demosaiced");
        output(x, y, c) = select(
            algorithm == 3, lmmse_output(x, y, c),
            algorithm == 2, ahd_output(x, y, c),
            algorithm == 1, vhg_output(x, y, c),
            simple_output(x, y, c)
        );
    }
};

// --- Method Implementations ---

void DemosaicBuilder::build_simple(Func result, Func deinterleaved, Var x, Var y, Var c) {
    Func g_gr("g_gr_s"), r_r("r_r_s"), b_b("b_b_s"), g_gb("g_gb_s");
    g_gr(qx, qy) = deinterleaved(qx, qy, 0); r_r(qx, qy)  = deinterleaved(qx, qy, 1);
    b_b(qx, qy) = deinterleaved(qx, qy, 2); g_gb(qx, qy) = deinterleaved(qx, qy, 3);
    
    Func g_at_r("g_at_r_s"), g_at_b("g_at_b_s");
    g_at_r(qx, qy) = (deinterleaved(qx, qy - 1, 3) + deinterleaved(qx, qy, 3) + deinterleaved(qx + 1, qy, 0) + deinterleaved(qx, qy, 0)) / 4.f;
    g_at_b(qx, qy) = (deinterleaved(qx, qy + 1, 0) + deinterleaved(qx, qy, 0) + deinterleaved(qx - 1, qy, 3) + deinterleaved(qx, qy, 3)) / 4.f;
    quarter_res_intermediates.push_back(g_at_r);
    quarter_res_intermediates.push_back(g_at_b);

    Func r_at_g("r_at_g_s"), b_at_g("b_at_g_s"), r_at_b("r_at_b_s"), b_at_r("b_at_r_s");
    r_at_g(qx, qy) = (r_r(qx - 1, qy) + r_r(qx, qy)) / 2.f;
    b_at_g(qx, qy) = (b_b(qx, qy - 1) + b_b(qx, qy)) / 2.f;
    r_at_b(qx, qy) = (r_r(qx - 1, qy) + r_r(qx, qy) + r_r(qx, qy - 1) + r_r(qx - 1, qy - 1)) / 4.f;
    b_at_r(qx, qy) = (b_b(qx + 1, qy) + b_b(qx, qy) + b_b(qx, qy + 1) + b_b(qx + 1, qy + 1)) / 4.f;

    Func r_full = interleave(r_at_g, r_r, r_at_b, r_at_g);
    Func g_full = interleave(g_gr, g_at_r, g_at_b, g_gb);
    Func b_full = interleave(b_at_g, b_at_r, b_b, b_at_g);
    result(x, y, c) = mux(c, {r_full(x, y), g_full(x, y), b_full(x, y)});
}

void DemosaicBuilder::build_vhg(Func result, Func deinterleaved, Var x, Var y, Var c) {
    Func g_gr, r_r, b_b, g_gb;
    g_gr(qx, qy) = deinterleaved(qx, qy, 0); r_r(qx, qy)  = deinterleaved(qx, qy, 1);
    b_b(qx, qy) = deinterleaved(qx, qy, 2); g_gb(qx, qy) = deinterleaved(qx, qy, 3);
    Func g_at_r("g_at_r_vhg"), g_at_b("g_at_b_vhg");
    Expr g_v_r = (g_gb(qx, qy - 1) + g_gb(qx, qy)) * 0.5f;
    Expr g_h_r = (g_gr(qx + 1, qy) + g_gr(qx, qy)) * 0.5f;
    Expr grad_v_r = abs(g_gb(qx, qy - 1) - g_gb(qx, qy));
    Expr grad_h_r = abs(g_gr(qx + 1, qy) - g_gr(qx, qy));
    g_at_r(qx, qy) = select(grad_h_r < grad_v_r, g_h_r, g_v_r);
    Expr g_v_b = (g_gr(qx, qy + 1) + g_gr(qx, qy)) * 0.5f;
    Expr g_h_b = (g_gb(qx - 1, qy) + g_gb(qx, qy)) * 0.5f;
    Expr grad_v_b = abs(g_gr(qx, qy + 1) - g_gr(qx, qy));
    Expr grad_h_b = abs(g_gb(qx - 1, qy) - g_gb(qx, qy));
    g_at_b(qx, qy) = select(grad_h_b < grad_v_b, g_h_b, g_v_b);
    quarter_res_intermediates.push_back(g_at_r);
    quarter_res_intermediates.push_back(g_at_b);
    Func g_full = interleave(g_gr, g_at_r, g_at_b, g_gb);
    Func R_minus_G("R_minus_G_vhg"), B_minus_G("B_minus_G_vhg");
    R_minus_G(qx, qy) = r_r(qx, qy) - g_at_r(qx, qy);
    B_minus_G(qx, qy) = b_b(qx, qy) - g_at_b(qx, qy);
    Func R_minus_G_interp("R_minus_G_interp_vhg"), B_minus_G_interp("B_minus_G_interp_vhg");
    Expr R_G_h = (R_minus_G(qx, qy) + R_minus_G(qx-1, qy)) * 0.5f;
    Expr R_G_v = (R_minus_G(qx, qy) + R_minus_G(qx, qy-1)) * 0.5f;
    R_minus_G_interp(qx, qy) = select(absd(R_minus_G(qx, qy), R_minus_G(qx-1, qy)) < absd(R_minus_G(qx, qy), R_minus_G(qx, qy-1)), R_G_h, R_G_v);
    Expr B_G_h = (B_minus_G(qx, qy) + B_minus_G(qx-1, qy)) * 0.5f;
    Expr B_G_v = (B_minus_G(qx, qy) + B_minus_G(qx, qy-1)) * 0.5f;
    B_minus_G_interp(qx, qy) = select(absd(B_minus_G(qx, qy), B_minus_G(qx-1, qy)) < absd(B_minus_G(qx, qy), B_minus_G(qx, qy-1)), B_G_h, B_G_v);
    Func R_G_full = interleave(R_minus_G_interp, R_minus_G, R_minus_G_interp, R_minus_G_interp);
    Func B_G_full = interleave(B_minus_G_interp, B_minus_G_interp, B_minus_G, B_minus_G_interp);
    Func r_full, b_full;
    r_full(x, y) = R_G_full(x, y) + g_full(x, y);
    b_full(x, y) = B_G_full(x, y) + g_full(x, y);
    result(x, y, c) = mux(c, {r_full(x, y), g_full(x, y), b_full(x, y)});
}

void DemosaicBuilder::build_ahd(Func result, Func deinterleaved, Var x, Var y, Var c) {
    Func g_gr, r_r, b_b, g_gb;
    g_gr(qx, qy) = deinterleaved(qx, qy, 0); r_r(qx, qy)  = deinterleaved(qx, qy, 1);
    b_b(qx, qy) = deinterleaved(qx, qy, 2); g_gb(qx, qy) = deinterleaved(qx, qy, 3);
    Func g_at_r("g_at_r_ahd"), g_at_b("g_at_b_ahd");
    Expr g_v_r = (g_gb(qx, qy - 1) + g_gb(qx, qy)) * 0.5f; Expr g_h_r = (g_gr(qx + 1, qy) + g_gr(qx, qy)) * 0.5f;
    g_at_r(qx, qy) = select(abs(g_gb(qx, qy - 1) - g_gb(qx, qy)) < abs(g_gr(qx + 1, qy) - g_gr(qx, qy)), g_v_r, g_h_r);
    Expr g_v_b = (g_gr(qx, qy + 1) + g_gr(qx, qy)) * 0.5f; Expr g_h_b = (g_gb(qx - 1, qy) + g_gb(qx, qy)) * 0.5f;
    g_at_b(qx, qy) = select(abs(g_gr(qx, qy + 1) - g_gr(qx, qy)) < abs(g_gb(qx - 1, qy) - g_gb(qx, qy)), g_v_b, g_h_b);
    quarter_res_intermediates.push_back(g_at_r); quarter_res_intermediates.push_back(g_at_b);
    Func g_full = interleave(g_gr, g_at_r, g_at_b, g_gb);
    Func R_minus_G("R_minus_G_ahd"), B_minus_G("B_minus_G_ahd");
    R_minus_G(qx, qy) = r_r(qx, qy) - g_at_r(qx, qy);
    B_minus_G(qx, qy) = b_b(qx, qy) - g_at_b(qx, qy);
    Func rg_interp("rg_interp_ahd"), bg_interp("bg_interp_ahd");
    const float eps = 1e-6f;
    Func R_m_G_c = Halide::BoundaryConditions::repeat_edge(R_minus_G, {{0, width_expr/2}, {0, height_expr/2}});
    Func B_m_G_c = Halide::BoundaryConditions::repeat_edge(B_minus_G, {{0, width_expr/2}, {0, height_expr/2}});
    Expr h_grad = absd(g_full(x-1, y), g_full(x+1, y)); Expr v_grad = absd(g_full(x, y-1), g_full(x, y+1));
    Expr h_weight = 1.0f / (eps + h_grad); Expr v_weight = 1.0f / (eps + v_grad);
    Expr rg_h = (R_m_G_c((x+1)/2, y/2) + R_m_G_c((x-1)/2, y/2)) / 2.f;
    Expr rg_v = (R_m_G_c(x/2, (y+1)/2) + R_m_G_c(x/2, (y-1)/2)) / 2.f;
    rg_interp(x, y) = (rg_h * h_weight + rg_v * v_weight) / (h_weight + v_weight);
    Expr bg_h = (B_m_G_c((x+1)/2, y/2) + B_m_G_c((x-1)/2, y/2)) / 2.f;
    Expr bg_v = (B_m_G_c(x/2, (y+1)/2) + B_m_G_c(x/2, (y-1)/2)) / 2.f;
    bg_interp(x, y) = (bg_h * h_weight + bg_v * v_weight) / (h_weight + v_weight);
    full_res_intermediates.push_back(rg_interp); full_res_intermediates.push_back(bg_interp);
    Func R_G_full, B_G_full;
    Expr is_r = (y%2==0) && (x%2!=0); Expr is_b = (y%2!=0) && (x%2==0);
    R_G_full(x, y) = select(is_r, R_minus_G(x/2, y/2), rg_interp(x,y));
    B_G_full(x, y) = select(is_b, B_minus_G(x/2, y/2), bg_interp(x,y));
    Func r_full, b_full;
    r_full(x, y) = g_full(x, y) + R_G_full(x, y); b_full(x, y) = g_full(x, y) + B_G_full(x, y);
    result(x, y, c) = mux(c, {r_full(x, y), g_full(x, y), b_full(x, y)});
}

void DemosaicBuilder::build_lmmse(Func result, Func deinterleaved, Var x, Var y, Var c) {
    Func vhg_guess("vhg_for_lmmse");
    build_vhg(vhg_guess, deinterleaved, x, y, c);
    
    Func current_rgb = vhg_guess;
    const int iterations = 2;
    for (int i = 0; i < iterations; i++) {
        Func refined_rgb("refined_rgb_" + std::to_string(i));
        Func clamped_current = Halide::BoundaryConditions::repeat_edge(current_rgb, {{0, width_expr}, {0, height_expr}, {0, 3}});
        Func synthetic_bayer("synthetic_bayer_" + std::to_string(i));
        // For GRBG pattern, Green is at (0,0), Red at (0,1), Blue at (1,0), Green at (1,1) in a 2x2 bayer quad.
        // We need to map from full-res (x,y) to channel index.
        // G (0): even, even. R (1): even, odd. B (2): odd, even. G (3): odd, odd.
        Expr is_y_even = (y % 2 == 0);
        Expr is_x_even = (x % 2 == 0);
        Expr c_idx = select(is_y_even,
                            select(is_x_even, 0, 1), // G, R
                            select(is_x_even, 2, 3)); // B, G
        synthetic_bayer(x,y) = clamped_current(x, y, c_idx);

        Func original_bayer("original_bayer_fullres" + std::to_string(i));
        Func g_gr, r_r, b_b, g_gb;
        g_gr(qx, qy) = deinterleaved(qx, qy, 0); r_r(qx, qy) = deinterleaved(qx, qy, 1);
        b_b(qx, qy) = deinterleaved(qx, qy, 2); g_gb(qx, qy) = deinterleaved(qx, qy, 3);
        original_bayer = interleave(g_gr, r_r, b_b, g_gb);
        Func error("error_" + std::to_string(i));
        error(x,y) = original_bayer(x,y) - synthetic_bayer(x,y);
        Func blurred_error("blurred_error_" + std::to_string(i));
        Func error_clamped = Halide::BoundaryConditions::repeat_edge(error, {{0, width_expr}, {0, height_expr}});
        RDom r_blur(-2, 5, -2, 5);
        blurred_error(x, y) = sum(error_clamped(x + r_blur.x, y + r_blur.y)) / 25.f;
        refined_rgb(x, y, c) = current_rgb(x, y, c) + blurred_error(x, y);
        full_res_intermediates.push_back(refined_rgb);
        current_rgb = refined_rgb;
    }
    result(x, y, c) = current_rgb(x, y, c);
}

#endif // STAGE_DEMOSAIC_H
