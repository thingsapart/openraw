#ifndef DEMOSAIC_RI_H
#define DEMOSAIC_RI_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include <vector>

// Implements Residual Interpolation demosaicing.
// This is a high-quality algorithm known for its sharpness. It works by
// making a simple guess (tentative interpolation), calculating the error (residual)
// where the true values are known, interpolating that error map, and then
// adding the interpolated error back to the initial guess for a final result.
template<typename T>
class DemosaicRI_T {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;
    Halide::Var qx, qy;

    DemosaicRI_T(Halide::Func deinterleaved, Halide::Var x_full, Halide::Var y_full, Halide::Var c_full) : qx("ri_qx"), qy("ri_qy") {
        using namespace Halide;
        using namespace Halide::ConciseCasts;
        
        Halide::Type proc_type = deinterleaved.type();

        Func deinterleaved_f("deinterleaved_f_ri");
        deinterleaved_f(qx, qy, c_full) = cast<float>(deinterleaved(qx, qy, c_full));

        // --- Stage 1: High-quality Green channel via Residual Interpolation
        Func g_final("g_final_ri");
        {
            // Tentative bilinear interpolation of G at R and B sites
            Func g_tentative_r("g_tentative_r");
            g_tentative_r(qx, qy) = avg(deinterleaved_f(qx, qy, 0), deinterleaved_f(qx + 1, qy, 0),
                                        deinterleaved_f(qx, qy, 3), deinterleaved_f(qx, qy - 1, 3));
            Func g_tentative_b("g_tentative_b");
            g_tentative_b(qx, qy) = avg(deinterleaved_f(qx, qy, 0), deinterleaved_f(qx, qy + 1, 0),
                                        deinterleaved_f(qx, qy, 3), deinterleaved_f(qx - 1, qy, 3));

            // Calculate residuals where G is known
            Func g_residual_gr("g_residual_gr"), g_residual_gb("g_residual_gb");
            g_residual_gr(qx, qy) = deinterleaved_f(qx, qy, 0) - avg(g_tentative_r(qx, qy), g_tentative_r(qx - 1, qy));
            g_residual_gb(qx, qy) = deinterleaved_f(qx, qy, 3) - avg(g_tentative_b(qx, qy), g_tentative_b(qx, qy + 1));
            
            // Interpolate the residuals
            Func g_residual_interp_r("g_residual_interp_r"), g_residual_interp_b("g_residual_interp_b");
            g_residual_interp_r(qx, qy) = avg(g_residual_gr(qx, qy), g_residual_gr(qx - 1, qy));
            g_residual_interp_b(qx, qy) = avg(g_residual_gb(qx, qy), g_residual_gb(qx, qy + 1));

            // Add interpolated residuals back to the tentative estimates
            Func g_corrected_r("g_corrected_r"), g_corrected_b("g_corrected_b");
            g_corrected_r(qx, qy) = g_tentative_r(qx, qy) + g_residual_interp_r(qx, qy);
            g_corrected_b(qx, qy) = g_tentative_b(qx, qy) + g_residual_interp_b(qx, qy);

            // Interleave to create the final green channel
            g_final(x_full, y_full) = mux(y_full % 2,
                                          {mux(x_full % 2, {deinterleaved_f(x_full/2, y_full/2, 0), g_corrected_r(x_full/2, y_full/2)}),
                                           mux(x_full % 2, {g_corrected_b(x_full/2, y_full/2), deinterleaved_f(x_full/2, y_full/2, 3)})});
        }
        intermediates.push_back(g_final);

        // --- Stage 2: R/B channels using G_final and color differences
        Func cd_r("cd_r"), cd_b("cd_b");
        cd_r(qx, qy) = deinterleaved_f(qx, qy, 1) - deinterleaved_f(qx, qy, 0);
        cd_b(qx, qy) = deinterleaved_f(qx, qy, 2) - deinterleaved_f(qx, qy, 3);
        intermediates.push_back(cd_r);
        intermediates.push_back(cd_b);

        // Interpolate color differences
        Func cd_r_interp("cd_r_interp"), cd_b_interp("cd_b_interp");
        cd_r_interp(x_full, y_full) = avg(cd_r(x_full/2, y_full/2), cd_r(x_full/2-1, y_full/2),
                                          cd_r(x_full/2, y_full/2-1), cd_r(x_full/2-1, y_full/2-1));
        cd_b_interp(x_full, y_full) = avg(cd_b(x_full/2, y_full/2), cd_b(x_full/2-1, y_full/2),
                                          cd_b(x_full/2, y_full/2-1), cd_b(x_full/2-1, y_full/2-1));
        intermediates.push_back(cd_r_interp);
        intermediates.push_back(cd_b_interp);

        Func r_final_f("r_final_f"), b_final_f("b_final_f");
        r_final_f(x_full, y_full) = g_final(x_full, y_full) + cd_r_interp(x_full, y_full);
        b_final_f(x_full, y_full) = g_final(x_full, y_full) + cd_b_interp(x_full, y_full);

        output = Func("demosaic_ri");
        Expr r_final = proc_type_sat<T>(r_final_f(x_full, y_full));
        Expr g_final_out = proc_type_sat<T>(g_final(x_full, y_full));
        Expr b_final = proc_type_sat<T>(b_final_f(x_full, y_full));
        output(x_full, y_full, c_full) = cast(proc_type, mux(c_full, {r_final, g_final_out, b_final}));
    }
};

#endif // DEMOSAIC_RI_H
