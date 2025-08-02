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

    // A clamp for convenience
    static const float_t max_val;

    // Helper to interleave two half-resolution Funcs into a single full-resolution one.
    Func interleave(Func a, Func b, Func c, Func d) {
        Var x("ix"), y("iy");
        Func out("interleaved_demosaic");
        Expr is_x_even = (x % 2 == 0);
        Expr is_y_even = (y % 2 == 0);
        Expr half_x = x / 2;
        Expr half_y = y / 2;
        out(x, y) = select(is_y_even,
                           select(is_x_even, a(half_x, half_y), b(half_x, half_y)),
                           select(is_x_even, c(half_x, half_y), d(half_x, half_y)));
        return out;
    }

    void build_simple(Func deinterleaved, Var x, Var y, Var c) {
        // This is a Float(32) implementation of the original demosaic algorithm.
        // Operating in float prevents integer underflow, which is a major source of artifacts.
        Func g_gr, r_r, b_b, g_gb;
        g_gr(qx, qy) = deinterleaved(qx, qy, 0);
        r_r(qx, qy)  = deinterleaved(qx, qy, 1);
        b_b(qx, qy)  = deinterleaved(qx, qy, 2);
        g_gb(qx, qy) = deinterleaved(qx, qy, 3);

        // Interpolate green at red and blue sites
        Func g_at_r("g_at_r"), g_at_b("g_at_b");
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

        // Interpolate R and B at green sites, using green correction
        Func r_at_gr, b_at_gr, r_at_gb, b_at_gb;
        Expr r_avg = (r_r(qx - 1, qy) + r_r(qx, qy)) * 0.5f;
        Expr g_avg = (g_at_r(qx, qy) + g_at_r(qx-1, qy)) * 0.5f;
        r_at_gr(qx, qy) = r_avg + g_gr(qx, qy) - g_avg;

        Expr b_avg = (b_b(qx, qy) + b_b(qx, qy-1)) * 0.5f;
        g_avg = (g_at_b(qx, qy) + g_at_b(qx, qy-1)) * 0.5f;
        b_at_gr(qx, qy) = b_avg + g_gr(qx, qy) - g_avg;
        
        r_avg = (r_r(qx, qy) + r_r(qx, qy+1)) * 0.5f;
        g_avg = (g_at_r(qx, qy) + g_at_r(qx, qy+1)) * 0.5f;
        r_at_gb(qx, qy) = r_avg + g_gb(qx, qy) - g_avg;

        b_avg = (b_b(qx, qy) + b_b(qx+1, qy)) * 0.5f;
        g_avg = (g_at_b(qx, qy) + g_at_b(qx+1, qy)) * 0.5f;
        b_at_gb(qx, qy) = b_avg + g_gb(qx, qy) - g_avg;

        // Interpolate R at B sites and B at R sites
        Func r_at_b, b_at_r;
        Expr r_p_b = (r_r(qx, qy) + r_r(qx-1, qy+1)) * 0.5f;
        Expr g_p_b = (g_at_r(qx, qy) + g_at_r(qx-1, qy+1)) * 0.5f;
        Expr r_n_b = (r_r(qx-1, qy) + r_r(qx, qy+1)) * 0.5f;
        Expr g_n_b = (g_at_r(qx-1, qy) + g_at_r(qx, qy+1)) * 0.5f;
        r_at_b(qx, qy) = select(abs(r_p_b-g_p_b) < abs(r_n_b-g_n_b), r_p_b + g_at_b(qx, qy) - g_p_b, r_n_b + g_at_b(qx, qy) - g_n_b);

        Expr b_p_r = (b_b(qx, qy) + b_b(qx+1, qy-1)) * 0.5f;
        Expr g_p_r = (g_at_b(qx, qy) + g_at_b(qx+1, qy-1)) * 0.5f;
        Expr b_n_r = (b_b(qx+1, qy) + b_b(qx, qy-1)) * 0.5f;
        Expr g_n_r = (g_at_b(qx+1, qy) + g_at_b(qx, qy-1)) * 0.5f;
        b_at_r(qx, qy) = select(abs(b_p_r-g_p_r) < abs(b_n_r-g_n_r), b_p_r + g_at_r(qx, qy) - g_p_r, b_n_r + g_at_r(qx, qy) - g_n_r);

        // Interleave the channels back to full resolution
        Func r_full = interleave(r_at_gr, r_r, r_at_b, r_at_gb);
        Func g_full = interleave(g_gr, g_at_r, g_at_b, g_gb);
        Func b_full = interleave(b_at_gr, b_at_r, b_b, b_at_gb);
        
        output(x, y, c) = mux(c, {r_full(x, y), g_full(x, y), b_full(x, y)});
    }
    
    void build_vhg(Func deinterleaved, Var x, Var y, Var c) {
        // A VHG (Variable Hue Gradient) Demosaic implementation.
        // It works by interpolating color differences (hue) instead of absolute values,
        // which helps reduce zippered artifacts along edges.
        Func g_gr, r_r, b_b, g_gb;
        g_gr(qx, qy) = deinterleaved(qx, qy, 0);
        r_r(qx, qy)  = deinterleaved(qx, qy, 1);
        b_b(qx, qy)  = deinterleaved(qx, qy, 2);
        g_gb(qx, qy) = deinterleaved(qx, qy, 3);
        
        // 1. Interpolate G everywhere first (same as simple method)
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

        // 2. Create hue maps (color differences) at R and B sites
        Func R_minus_G("R_minus_G"), B_minus_G("B_minus_G");
        R_minus_G(qx, qy) = r_r(qx, qy) - g_at_r(qx, qy);
        B_minus_G(qx, qy) = b_b(qx, qy) - g_at_b(qx, qy);

        // 3. Interpolate hue maps everywhere
        Func R_minus_G_interp("R_minus_G_interp"), B_minus_G_interp("B_minus_G_interp");
        Expr R_G_h = (R_minus_G(qx, qy) + R_minus_G(qx-1, qy)) * 0.5f;
        Expr R_G_v = (R_minus_G(qx, qy) + R_minus_G(qx, qy-1)) * 0.5f;
        Expr R_G_grad_h = absd(R_minus_G(qx, qy), R_minus_G(qx-1, qy));
        Expr R_G_grad_v = absd(R_minus_G(qx, qy), R_minus_G(qx, qy-1));
        R_minus_G_interp(qx, qy) = select(R_G_grad_h < R_G_grad_v, R_G_h, R_G_v);

        Expr B_G_h = (B_minus_G(qx, qy) + B_minus_G(qx-1, qy)) * 0.5f;
        Expr B_G_v = (B_minus_G(qx, qy) + B_minus_G(qx, qy-1)) * 0.5f;
        Expr B_G_grad_h = absd(B_minus_G(qx, qy), B_minus_G(qx-1, qy));
        Expr B_G_grad_v = absd(B_minus_G(qx, qy), B_minus_G(qx, qy-1));
        B_minus_G_interp(qx, qy) = select(B_G_grad_h < B_G_grad_v, B_G_h, B_G_v);
        
        Func R_minus_G_full = interleave(R_minus_G_interp, R_minus_G, R_minus_G_interp, R_minus_G_interp);
        Func B_minus_G_full = interleave(B_minus_G_interp, B_minus_G_interp, B_minus_G, B_minus_G_interp);

        // 4. Reconstruct R and B by adding back the full-res Green channel
        Func r_full, b_full;
        r_full(x, y) = R_minus_G_full(x, y) + g_full(x, y);
        b_full(x, y) = B_minus_G_full(x, y) + g_full(x, y);

        output(x, y, c) = mux(c, {r_full(x, y), g_full(x, y), b_full(x, y)});
    }

public:
    // The final output Func of this stage
    Func output;
    // The full-resolution algorithm outputs, exposed for scheduling
    Func simple_output;
    Func vhg_output;
    // The quarter-resolution intermediate Funcs that need to be scheduled by the parent
    std::vector<Func> quarter_res_intermediates;
    // The Vars used to define the quarter-sized intermediates, exposed for scheduling
    Var qx, qy;

    DemosaicBuilder(Func deinterleaved, Var x, Var y, Var c, Halide::Expr algorithm, Halide::Expr width, Halide::Expr height)
        : qx("d_qx"), qy("d_qy")
    {
        // The input deinterleaved Func is on a quarter-sized domain. To handle
        // boundary conditions robustly for different algorithms, we'll clamp it.
        // The width and height are passed in as Exprs from the generator.
        Region bounds = {{Expr(0), width}, {Expr(0), height}, {Expr(0), 4}};
        Func deinterleaved_clamped = Halide::BoundaryConditions::repeat_edge(deinterleaved, bounds);
        
        // Initialize the public Func members that will hold the algorithm outputs
        this->simple_output = Func("demosaiced_simple");
        this->vhg_output = Func("demosaiced_vhg");
        
        // Build the simple path, which will define `this->simple_output`
        this->output = this->simple_output; // Redirect member
        build_simple(deinterleaved_clamped, x, y, c);
        std::vector<Func> simple_intermediates_list = this->quarter_res_intermediates;
        
        // Build the VHG path, which will define `this->vhg_output`
        this->output = this->vhg_output; // Redirect member
        this->quarter_res_intermediates.clear();
        build_vhg(deinterleaved_clamped, x, y, c);
        std::vector<Func> vhg_intermediates_list = this->quarter_res_intermediates;

        // Combine the lists of quarter-res intermediates for scheduling
        this->quarter_res_intermediates = simple_intermediates_list;
        this->quarter_res_intermediates.insert(this->quarter_res_intermediates.end(), vhg_intermediates_list.begin(), vhg_intermediates_list.end());

        // Define the final output Func which selects between the two algorithm outputs
        this->output = Func("demosaiced");
        this->output(x, y, c) = select(algorithm == 1, this->vhg_output(x, y, c),
                                                      this->simple_output(x, y, c));
    }
};

#endif // STAGE_DEMOSAIC_H
