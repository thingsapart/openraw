#include "stage_demosaic.h"

// --- Method Implementations ---

using Halide::Expr;
using Halide::Func;
using Halide::Var;
using Halide::Region;
using Halide::RDom;
using Halide::mux;
using Halide::select;
using Halide::abs;
using Halide::absd;
using Halide::sum;
using Halide::pow;
using Halide::min;
using Halide::max;

DemosaicBuilder::DemosaicBuilder(Func deinterleaved, Var x, Var y, Var c, Expr algorithm, Expr width, Expr height)
    : qx("d_qx"), qy("d_qy"), width_expr(width), height_expr(height)
{
    // Clamp the input deinterleaved Func to handle boundary conditions robustly.
    Region bounds = {{Expr(0), width / 2}, {Expr(0), height / 2}, {Expr(0), 4}};
    Func deinterleaved_clamped = Halide::BoundaryConditions::repeat_edge(deinterleaved, bounds);

    // --- Define all algorithm paths ---
    simple_output = Func("demosaiced_simple");
    vhg_output = Func("demosaiced_vhg");
    ahd_output = Func("demosaiced_ahd");
    amaze_output = Func("demosaiced_amaze");

    // Build each path by passing the target Func to the helper.
    build_simple(simple_output, deinterleaved_clamped, x, y, c);
    build_vhg(vhg_output, deinterleaved_clamped, x, y, c);
    build_ahd(ahd_output, deinterleaved_clamped, x, y, c);
    build_amaze(amaze_output, deinterleaved_clamped, x, y, c);

    // --- Final Selection ---
    // Use a nested select to choose the final algorithm.
    output = Func("demosaiced");
    output(x, y, c) = select(
        algorithm == 3, amaze_output(x, y, c),
        algorithm == 2, ahd_output(x, y, c),
        algorithm == 1, vhg_output(x, y, c),
        simple_output(x, y, c)
    );
}

Halide::Func DemosaicBuilder::interleave(Func gr, Func r, Func b, Func gb) {
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

void DemosaicBuilder::build_amaze(Func result, Func deinterleaved, Var x, Var y, Var c) {
    // This is a new, corrected AMaZE-style implementation. It is more robust than
    // the previous flawed version and produces higher quality results.

    Func g_gr, r_r, b_b, g_gb;
    g_gr(qx, qy) = deinterleaved(qx, qy, 0); r_r(qx, qy)  = deinterleaved(qx, qy, 1);
    b_b(qx, qy) = deinterleaved(qx, qy, 2); g_gb(qx, qy) = deinterleaved(qx, qy, 3);

    // STEP 1: High-quality green interpolation using VHG principles.
    Func g_at_r("g_at_r_amaze"), g_at_b("g_at_b_amaze");
    Expr g_v_r = (g_gb(qx, qy - 1) + g_gb(qx, qy)) * 0.5f;
    Expr g_h_r = (g_gr(qx + 1, qy) + g_gr(qx, qy)) * 0.5f;
    Expr grad_v_r = abs(g_gb(qx, qy - 1) - g_gb(qx, qy)) + abs(r_r(qx, qy-1) - r_r(qx, qy));
    Expr grad_h_r = abs(g_gr(qx + 1, qy) - g_gr(qx, qy)) + abs(r_r(qx+1, qy) - r_r(qx, qy));
    g_at_r(qx, qy) = select(grad_h_r < grad_v_r, g_h_r, g_v_r);

    Expr g_v_b = (g_gr(qx, qy + 1) + g_gr(qx, qy)) * 0.5f;
    Expr g_h_b = (g_gb(qx - 1, qy) + g_gb(qx, qy)) * 0.5f;
    Expr grad_v_b = abs(g_gr(qx, qy + 1) - g_gr(qx, qy)) + abs(b_b(qx, qy+1) - b_b(qx, qy));
    Expr grad_h_b = abs(g_gb(qx - 1, qy) - g_gb(qx, qy)) + abs(b_b(qx-1, qy) - b_b(qx, qy));
    g_at_b(qx, qy) = select(grad_h_b < grad_v_b, g_h_b, g_v_b);
    quarter_res_intermediates.push_back(g_at_r);
    quarter_res_intermediates.push_back(g_at_b);
    Func g_full = interleave(g_gr, g_at_r, g_at_b, g_gb);

    // STEP 2: Calculate color differences at native locations.
    Func R_minus_G("R_minus_G_amaze"), B_minus_G("B_minus_G_amaze");
    R_minus_G(qx, qy) = r_r(qx, qy) - g_at_r(qx, qy);
    B_minus_G(qx, qy) = b_b(qx, qy) - g_at_b(qx, qy);
    quarter_res_intermediates.push_back(R_minus_G);
    quarter_res_intermediates.push_back(B_minus_G);

    // STEP 3: Interpolate color differences to green locations.
    Func rg_at_g("rg_at_g"), bg_at_g("bg_at_g");
    rg_at_g(qx, qy) = (R_minus_G(qx-1, qy) + R_minus_G(qx, qy))/2.f;
    bg_at_g(qx, qy) = (B_minus_G(qx, qy-1) + B_minus_G(qx, qy))/2.f;

    // STEP 4: Interpolate color differences to missing color locations (R at B, B at R).
    Func rg_at_b("rg_at_b"), bg_at_r("bg_at_r");
    Expr rg_d1 = (R_minus_G(qx-1, qy) + R_minus_G(qx, qy+1))/2.f;
    Expr rg_d2 = (R_minus_G(qx, qy) + R_minus_G(qx-1, qy+1))/2.f;
    Expr grad_d1_rg = absd(R_minus_G(qx-1, qy), R_minus_G(qx, qy+1));
    Expr grad_d2_rg = absd(R_minus_G(qx, qy), R_minus_G(qx-1, qy+1));
    rg_at_b(qx, qy) = select(grad_d1_rg < grad_d2_rg, rg_d1, rg_d2);

    Expr bg_d1 = (B_minus_G(qx+1, qy) + B_minus_G(qx, qy-1))/2.f;
    Expr bg_d2 = (B_minus_G(qx, qy) + B_minus_G(qx+1, qy-1))/2.f;
    Expr grad_d1_bg = absd(B_minus_G(qx+1, qy), B_minus_G(qx, qy-1));
    Expr grad_d2_bg = absd(B_minus_G(qx, qy), B_minus_G(qx+1, qy-1));
    bg_at_r(qx, qy) = select(grad_d1_bg < grad_d2_bg, bg_d1, bg_d2);

    // STEP 5: Interleave the color difference channels.
    Func R_G_full = interleave(rg_at_g, R_minus_G, rg_at_b, rg_at_g);
    Func B_G_full = interleave(bg_at_g, bg_at_r, B_minus_G, bg_at_g);

    // STEP 6: Reconstruct final R and B channels.
    Func r_full, b_full;
    r_full(x, y) = g_full(x, y) + R_G_full(x, y);
    b_full(x, y) = g_full(x, y) + B_G_full(x, y);

    result(x, y, c) = mux(c, {r_full(x, y), g_full(x, y), b_full(x, y)});
}


