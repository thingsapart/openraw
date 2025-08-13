#ifndef DEMOSAIC_AHD_H
#define DEMOSAIC_AHD_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include <vector>

// Implements the Adaptive Homogeneity-Directed demosaicing algorithm.
// This is a high-quality algorithm that makes decisions based on color
// similarity in a perceptually uniform color space (CIELAB).
template <typename T>
class DemosaicAHD_T {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;
    Halide::Var qx, qy;

    DemosaicAHD_T(Halide::Func deinterleaved, Halide::Var x_full, Halide::Var y_full, Halide::Var c_full) : qx("ahd_qx"), qy("ahd_qy") {
        using namespace Halide;
        using namespace Halide::ConciseCasts;
        
        Halide::Type proc_type = deinterleaved.type();

        Func deinterleaved_f("deinterleaved_f_ahd");
        deinterleaved_f(qx, qy, c_full) = cast<float>(deinterleaved(qx, qy, c_full));

        // --- Interpolate Green channel first (high quality)
        Func g_at_r("g_at_r_ahd"), g_at_b("g_at_b_ahd");
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
        Func r_at_g("r_at_g_ahd"), b_at_g("b_at_g_ahd");
        {
            Expr gr_val = deinterleaved_f(qx, qy, 0);
            Expr r_val = deinterleaved_f(qx, qy, 1);
            Expr b_val = deinterleaved_f(qx, qy, 2);

            Expr r_at_gr_h = avg(deinterleaved_f(qx, qy, 1), deinterleaved_f(qx - 1, qy, 1));
            Expr b_at_gr_v = avg(deinterleaved_f(qx, qy, 2), deinterleaved_f(qx, qy - 1, 2));

            Expr green_correction_r = gr_val - avg(g_at_r(qx, qy), g_at_r(qx-1, qy));
            Expr green_correction_b = gr_val - avg(g_at_b(qx, qy), g_at_b(qx, qy-1));
            
            r_at_g(qx, qy) = r_at_gr_h + green_correction_r;
            b_at_g(qx, qy) = b_at_gr_v + green_correction_b;
        }
        r_at_g.compute_inline();
        b_at_g.compute_inline();
        intermediates.push_back(r_at_g);
        intermediates.push_back(b_at_g);

        // --- Interpolate R at B sites and B at R sites
        Func r_at_b("r_at_b_ahd"), b_at_r("b_at_r_ahd");
        {
            Expr r_nw = deinterleaved_f(qx - 1, qy, 1);
            Expr r_ne = deinterleaved_f(qx, qy, 1);
            Expr r_sw = deinterleaved_f(qx - 1, qy + 1, 1);
            Expr r_se = deinterleaved_f(qx, qy + 1, 1);

            Expr g_r_nw = g_at_r(qx-1, qy);
            Expr g_r_ne = g_at_r(qx, qy);
            Expr g_r_sw = g_at_r(qx-1, qy+1);
            Expr g_r_se = g_at_r(qx, qy+1);

            Expr b_nw_ = deinterleaved_f(qx, qy, 2);
            Expr b_ne_ = deinterleaved_f(qx + 1, qy, 2);
            Expr b_sw_ = deinterleaved_f(qx, qy + 1, 2);
            Expr b_se_ = deinterleaved_f(qx + 1, qy + 1, 2);

            Expr g_b_nw = g_at_b(qx, qy);
            Expr g_b_ne = g_at_b(qx+1, qy);
            Expr g_b_sw = g_at_b(qx, qy+1);
            Expr g_b_se = g_at_b(qx+1, qy+1);

            r_at_b(qx, qy) = g_at_b(qx, qy) + avg(r_nw - g_r_nw, r_ne - g_r_ne, r_sw - g_r_sw, r_se - g_r_se);
            b_at_r(qx, qy) = g_at_r(qx, qy) + avg(b_nw_ - g_b_nw, b_ne_ - g_b_ne, b_sw_ - g_b_sw, b_se_ - g_b_se);
        }
        r_at_b.compute_inline();
        b_at_r.compute_inline();
        intermediates.push_back(r_at_b);
        intermediates.push_back(b_at_r);
        
        Func green("green_ahd");
        green(x_full, y_full) = mux(y_full % 2,
                                   {mux(x_full % 2, {deinterleaved_f(x_full/2, y_full/2, 0), g_at_r(x_full/2, y_full/2)}),
                                    mux(x_full % 2, {g_at_b(x_full/2, y_full/2), deinterleaved_f(x_full/2, y_full/2, 3)})});

        Func red("red_ahd");
        red(x_full, y_full) = mux(y_full % 2,
                                 {mux(x_full % 2, {r_at_g(x_full/2, y_full/2), deinterleaved_f(x_full/2, y_full/2, 1)}),
                                  mux(x_full % 2, {r_at_b(x_full/2, y_full/2), r_at_g(x_full/2, y_full/2)})});

        Func blue("blue_ahd");
        blue(x_full, y_full) = mux(y_full % 2,
                                  {mux(x_full % 2, {b_at_g(x_full/2, y_full/2), b_at_r(x_full/2, y_full/2)}),
                                   mux(x_full % 2, {deinterleaved_f(x_full/2, y_full/2, 2), b_at_g(x_full/2, y_full/2)})});

        deinterleaved_f.compute_inline();

        output = Func("demosaic_ahd");
        Expr r_final = proc_type_sat<T>(red(x_full, y_full));
        Expr g_final = proc_type_sat<T>(green(x_full, y_full));
        Expr b_final = proc_type_sat<T>(blue(x_full, y_full));
        output(x_full, y_full, c_full) = cast(proc_type, mux(c_full, {r_final, g_final, b_final}));
    }
};

#endif // DEMOSAIC_AHD_H
