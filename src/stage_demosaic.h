#ifndef STAGE_DEMOSAIC_H
#define STAGE_DEMOSAIC_H

#include "Halide.h"
#include "halide_trace_config.h"
#include "pipeline_helpers.h"
#include <vector>
#include <string>
#include <algorithm>

namespace {

// Helper functions used by the Demosaic algorithm
using Halide::Expr;
using Halide::Func;
using Halide::cast;
using Halide::absd;
using Halide::mux;
using Halide::select;
using Halide::Var;

Func interleave_x(Func a, Func b) {
    Var x("ix_x"), y("ix_y");
    Func out("interleaved_x");
    out(x, y) = select((x % 2) == 0, a(x / 2, y), b(x / 2, y));
    return out;
}

Func interleave_y(Func a, Func b) {
    Var x("iy_x"), y("iy_y");
    Func out("interleaved_y");
    out(x, y) = select((y % 2) == 0, a(x, y / 2), b(x, y / 2));
    return out;
}
} // namespace

// DemosaicBuilder is a plain C++ class that constructs the Halide algorithm
// for the demosaic stage. It no longer contains scheduling logic itself.
class DemosaicBuilder {
public:
    // The final output Func of this stage
    Func output;
    // The intermediate Funcs that need to be scheduled by the parent
    std::vector<Func> intermediates;
    // The Vars used to define the quarter-sized intermediates, exposed for scheduling
    Var qx, qy;

    DemosaicBuilder(Func deinterleaved, Var x, Var y, Var c) : qx("d_qx"), qy("d_qy") {
#ifdef NO_DEMOSAIC
        output = Func("demosaiced_dummy");
        
        // Dummy stage: A real demosaic converts a (w/2, h/2, 4) image to a
        // (w, h, 3) image. This dummy must do the same to maintain pipeline
        // integrity. We simply take the R, G, B channels and ignore the second G.
        // The output size is implicitly handled by how Halide computes it.
        Expr r_channel = deinterleaved(x/2, y/2, 1);
        Expr g_channel = avg(deinterleaved(x/2, y/2, 0), deinterleaved(x/2, y/2, 3));
        Expr b_channel = deinterleaved(x/2, y/2, 2);
        output(x, y, c) = cast<uint16_t>(mux(c, {r_channel, g_channel, b_channel}));
        // No intermediates in the dummy version.

#else
        Func r_r("r_r"), g_gr("g_gr"), g_gb("g_gb"), b_b("b_b");
        g_gr(qx, qy) = deinterleaved(qx, qy, 0);
        r_r(qx, qy) = deinterleaved(qx, qy, 1);
        b_b(qx, qy) = deinterleaved(qx, qy, 2);
        g_gb(qx, qy) = deinterleaved(qx, qy, 3);

        Func g_r("g_r"), g_b("g_b");
        intermediates.push_back(g_r);
        intermediates.push_back(g_b);

        Expr gv_r = avg(g_gb(qx, qy - 1), g_gb(qx, qy));
        Expr gvd_r = absd(g_gb(qx, qy - 1), g_gb(qx, qy));
        Expr gh_r = avg(g_gr(qx + 1, qy), g_gr(qx, qy));
        Expr ghd_r = absd(g_gr(qx + 1, qy), g_gr(qx, qy));
        g_r(qx, qy) = select(ghd_r < gvd_r, gh_r, gv_r);

        Expr gv_b = avg(g_gr(qx, qy + 1), g_gr(qx, qy));
        Expr gvd_b = absd(g_gr(qx, qy + 1), g_gr(qx, qy));
        Expr gh_b = avg(g_gb(qx - 1, qy), g_gb(qx, qy));
        Expr ghd_b = absd(g_gb(qx - 1, qy), g_gb(qx, qy));
        g_b(qx, qy) = select(ghd_b < gvd_b, gh_b, gv_b);

        Func r_gr("r_gr"), b_gr("b_gr"), r_gb("r_gb"), b_gb("b_gb");
        Expr green_correction_r_gr = g_gr(qx, qy) - avg(g_r(qx, qy), g_r(qx - 1, qy));
        r_gr(qx, qy) = green_correction_r_gr + avg(r_r(qx - 1, qy), r_r(qx, qy));
        Expr green_correction_b_gr = g_gr(qx, qy) - avg(g_b(qx, qy), g_b(qx, qy - 1));
        b_gr(qx, qy) = green_correction_b_gr + avg(b_b(qx, qy), b_b(qx, qy - 1));
        Expr green_correction_r_gb = g_gb(qx, qy) - avg(g_r(qx, qy), g_r(qx, qy + 1));
        r_gb(qx, qy) = green_correction_r_gb + avg(r_r(qx, qy), r_r(qx, qy + 1));
        Expr green_correction_b_gb = g_gb(qx, qy) - avg(g_b(qx, qy), g_b(qx + 1, qy));
        b_gb(qx, qy) = green_correction_b_gb + avg(b_b(qx, qy), b_b(qx + 1, qy));

        Func r_b("r_b"), b_r("b_r");
        Expr green_correction_rp_b = g_b(qx, qy) - avg(g_r(qx, qy), g_r(qx - 1, qy + 1));
        Expr rp_b = green_correction_rp_b + avg(r_r(qx, qy), r_r(qx - 1, qy + 1));
        Expr rpd_b = absd(r_r(qx, qy), r_r(qx - 1, qy + 1));
        Expr green_correction_rn_b = g_b(qx, qy) - avg(g_r(qx - 1, qy), g_r(qx, qy + 1));
        Expr rn_b = green_correction_rn_b + avg(r_r(qx - 1, qy), r_r(qx, qy + 1));
        Expr rnd_b = absd(r_r(qx - 1, qy), r_r(qx, qy + 1));
        r_b(qx, qy) = select(rpd_b < rnd_b, rp_b, rn_b);
        Expr green_correction_bp_r = g_r(qx, qy) - avg(g_b(qx, qy), g_b(qx + 1, qy - 1));
        Expr bp_r = green_correction_bp_r + avg(b_b(qx, qy), b_b(qx + 1, qy - 1));
        Expr bpd_r = absd(b_b(qx, qy), b_b(qx + 1, qy - 1));
        Expr green_correction_bn_r = g_r(qx, qy) - avg(g_b(qx + 1, qy), g_b(qx, qy - 1));
        Expr bn_r = green_correction_bn_r + avg(b_b(qx + 1, qy), b_b(qx, qy - 1));
        Expr bnd_r = absd(b_b(qx + 1, qy), b_b(qx, qy - 1));
        b_r(qx, qy) = select(bpd_r < bnd_r, bp_r, bn_r);

        Func r_full("demosaic_r"), g_full("demosaic_g"), b_full("demosaic_b");
        r_full = interleave_y(interleave_x(r_gr, r_r), interleave_x(r_b, r_gb));
        g_full = interleave_y(interleave_x(g_gr, g_r), interleave_x(g_b, g_gb));
        b_full = interleave_y(interleave_x(b_gr, b_r), interleave_x(b_b, b_gb));

        output = Func("demosaiced");
        output(x, y, c) = cast<uint16_t>(mux(c, {r_full(x, y), g_full(x, y), b_full(x, y)}));
#endif
    }
};

#endif // STAGE_DEMOSAIC_H
