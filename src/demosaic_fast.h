#ifndef DEMOSAIC_FAST_H
#define DEMOSAIC_FAST_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include <vector>

// This class implements the original, faster demosaicing algorithm.
// It uses a high-quality green channel interpolation followed by a
// directionally-adaptive color difference interpolation for red and blue.
// It is significantly faster than AHD, LMMSE, or RI.
template <typename T>
class DemosaicFastT {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;
    Halide::Var qx, qy;

    DemosaicFastT(Halide::Func deinterleaved, Halide::Var x_full, Halide::Var y_full, Halide::Var c_full) : qx("fast_qx"), qy("fast_qy") {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        Halide::Type proc_type = deinterleaved.type();

        Func deinterleaved_f("deinterleaved_f_fast");
        deinterleaved_f(qx, qy, c_full) = cast<float>(deinterleaved(qx, qy, c_full));

        // --- High-quality Green interpolation
        Func g_at_r("g_at_r_fast"), g_at_b("g_at_b_fast");
        {
            Expr gb_cm1 = deinterleaved_f(qx, qy - 1, 3);
            Expr gb_c = deinterleaved_f(qx, qy, 3);
            Expr gr_c = deinterleaved_f(qx, qy, 0);
            Expr gr_cp1 = deinterleaved_f(qx + 1, qy, 0);

            Expr gv_r = avg(gb_cm1, gb_c);
            Expr gvd_r = absd(gb_cm1, gb_c);
            Expr gh_r = avg(gr_cp1, gr_c);
            Expr ghd_r = absd(gr_cp1, gr_c);
            g_at_r(qx, qy) = select(ghd_r < gvd_r, gh_r, gv_r);

            Expr gr_cp1_v = deinterleaved_f(qx, qy + 1, 0);
            Expr gb_cm1_h = deinterleaved_f(qx - 1, qy, 3);

            Expr gv_b = avg(gr_cp1_v, gr_c);
            Expr gvd_b = absd(gr_cp1_v, gr_c);
            Expr gh_b = avg(gb_cm1_h, gb_c);
            Expr ghd_b = absd(gb_cm1_h, gb_c);
            g_at_b(qx, qy) = select(ghd_b < gvd_b, gh_b, gv_b);
        }
        g_at_r.compute_inline();
        g_at_b.compute_inline();
        intermediates.push_back(g_at_r);
        intermediates.push_back(g_at_b);

        // --- Interpolate R/B at green sites by preserving color difference
        Func r_at_gr("r_at_gr_fast"), b_at_gr("b_at_gr_fast");
        Func r_at_gb("r_at_gb_fast"), b_at_gb("b_at_gb_fast");
        {
            Expr gr_val = deinterleaved_f(qx, qy, 0);
            Expr gb_val = deinterleaved_f(qx, qy, 3);
            Expr r_val = deinterleaved_f(qx, qy, 1);
            Expr b_val = deinterleaved_f(qx, qy, 2);

            Expr green_correction_r_gr = gr_val - avg(g_at_r(qx, qy), g_at_r(qx - 1, qy));
            r_at_gr(qx, qy) = green_correction_r_gr + avg(deinterleaved_f(qx - 1, qy, 1), r_val);

            Expr green_correction_b_gr = gr_val - avg(g_at_b(qx, qy), g_at_b(qx, qy - 1));
            b_at_gr(qx, qy) = green_correction_b_gr + avg(b_val, deinterleaved_f(qx, qy - 1, 2));

            Expr green_correction_r_gb = gb_val - avg(g_at_r(qx, qy), g_at_r(qx, qy + 1));
            r_at_gb(qx, qy) = green_correction_r_gb + avg(r_val, deinterleaved_f(qx, qy + 1, 1));

            Expr green_correction_b_gb = gb_val - avg(g_at_b(qx, qy), g_at_b(qx + 1, qy));
            b_at_gb(qx, qy) = green_correction_b_gb + avg(b_val, deinterleaved_f(qx + 1, qy, 2));
        }
        r_at_gr.compute_inline();
        b_at_gr.compute_inline();
        r_at_gb.compute_inline();
        b_at_gb.compute_inline();
        intermediates.insert(intermediates.end(), {r_at_gr, b_at_gr, r_at_gb, b_at_gb});

        // --- Interpolate R at B sites and B at R sites
        Func r_at_b("r_at_b_fast"), b_at_r("b_at_r_fast");
        {
            Expr rp_b = g_at_b(qx, qy) - avg(g_at_r(qx, qy), g_at_r(qx - 1, qy + 1));
            rp_b += avg(deinterleaved_f(qx, qy, 1), deinterleaved_f(qx-1, qy+1, 1));
            Expr rpd_b = absd(deinterleaved_f(qx, qy, 1), deinterleaved_f(qx - 1, qy + 1, 1));

            Expr rn_b = g_at_b(qx, qy) - avg(g_at_r(qx-1, qy), g_at_r(qx, qy+1));
            rn_b += avg(deinterleaved_f(qx-1, qy, 1), deinterleaved_f(qx, qy+1, 1));
            Expr rnd_b = absd(deinterleaved_f(qx - 1, qy, 1), deinterleaved_f(qx, qy + 1, 1));
            r_at_b(qx, qy) = select(rpd_b < rnd_b, rp_b, rn_b);

            Expr bp_r = g_at_r(qx, qy) - avg(g_at_b(qx, qy), g_at_b(qx + 1, qy - 1));
            bp_r += avg(deinterleaved_f(qx, qy, 2), deinterleaved_f(qx+1, qy-1, 2));
            Expr bpd_r = absd(deinterleaved_f(qx, qy, 2), deinterleaved_f(qx+1, qy-1, 2));

            Expr bn_r = g_at_r(qx, qy) - avg(g_at_b(qx+1, qy), g_at_b(qx, qy-1));
            bn_r += avg(deinterleaved_f(qx+1, qy, 2), deinterleaved_f(qx, qy-1, 2));
            Expr bnd_r = absd(deinterleaved_f(qx+1, qy, 2), deinterleaved_f(qx, qy-1, 2));
            b_at_r(qx, qy) = select(bpd_r < bnd_r, bp_r, bn_r);
        }
        r_at_b.compute_inline();
        b_at_r.compute_inline();
        intermediates.push_back(r_at_b);
        intermediates.push_back(b_at_r);

        // --- Final Assembly
        Func r_full("demosaic_r_fast"), g_full("demosaic_g_fast"), b_full("demosaic_b_fast");
        g_full(x_full, y_full) = mux(y_full % 2,
                                  {mux(x_full % 2, {deinterleaved_f(x_full/2, y_full/2, 0), g_at_r(x_full/2, y_full/2)}),
                                   mux(x_full % 2, {g_at_b(x_full/2, y_full/2), deinterleaved_f(x_full/2, y_full/2, 3)})});

        r_full(x_full, y_full) = mux(y_full % 2,
                                  {mux(x_full % 2, {r_at_gr(x_full/2, y_full/2), deinterleaved_f(x_full/2, y_full/2, 1)}),
                                   mux(x_full % 2, {r_at_b(x_full/2, y_full/2), r_at_gb(x_full/2, y_full/2)})});

        b_full(x_full, y_full) = mux(y_full % 2,
                                  {mux(x_full % 2, {b_at_gr(x_full/2, y_full/2), b_at_r(x_full/2, y_full/2)}),
                                   mux(x_full % 2, {deinterleaved_f(x_full/2, y_full/2, 2), b_at_gb(x_full/2, y_full/2)})});

        deinterleaved_f.compute_inline();

        output = Func("demosaic_fast");
        Expr r_final = proc_type_sat<T>(r_full(x_full, y_full));
        Expr g_final = proc_type_sat<T>(g_full(x_full, y_full));
        Expr b_final = proc_type_sat<T>(b_full(x_full, y_full));
        output(x_full, y_full, c_full) = cast(proc_type, mux(c_full, {r_final, g_final, b_final}));
    }
};

#endif // DEMOSAIC_FAST_H

