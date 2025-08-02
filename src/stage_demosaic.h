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
    Func interleave(Func gr, Func r, Func b, Func gb);

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

    // Constructor declaration
    DemosaicBuilder(Func deinterleaved, Var x, Var y, Var c, Expr algorithm, Expr width, Expr height);
};

#endif // STAGE_DEMOSAIC_H
