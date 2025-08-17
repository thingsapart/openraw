#ifndef STAGE_DEMOSAIC_H
#define STAGE_DEMOSAIC_H

#include "Halide.h"
#include <vector>
#include <string>

#include "demosaic_AHD.h"
#include "demosaic_LMMSE.h"
#include "demosaic_RI.h"
#include "demosaic_fast.h"

// This class acts as a dispatcher for multiple demosaicing algorithms.
// It is now templated to support different processing precisions.
// It instantiates all available algorithms and uses Halide's 'select'
// primitive to choose one at runtime based on a parameter. Halide's JIT
// compiler will then perform dead-code elimination on the inactive
// algorithm paths, resulting in zero runtime overhead for the selection.
template<typename T>
class DemosaicDispatcherT {
public:
    // The final, selected output Func
    Halide::Func output;

    // A collection of ALL intermediate Funcs from ALL possible algorithms.
    // This is needed so the generator can schedule them.
    std::vector<Halide::Func> all_intermediates;

    DemosaicDispatcherT(Halide::Func deinterleaved, Halide::Expr algo_id, Halide::Var x, Halide::Var y, Halide::Var c) {

        // --- Instantiate all demosaic algorithms ---

        // Algorithm 0: AHD
        DemosaicAHD_T<T> ahd_builder(deinterleaved, x, y, c);

        // Algorithm 1: LMMSE
        DemosaicLMMSE_T<T> lmmse_builder(deinterleaved, x, y, c);

        // Algorithm 2: RI
        DemosaicRI_T<T> ri_builder(deinterleaved, x, y, c);

        // Algorithm 3: Fast (the original algorithm)
        DemosaicFastT<T> fast_builder(deinterleaved, x, y, c);

        // --- Use 'select' to create the final dispatcher Func ---
        output = Halide::Func("demosaiced");
        output(x, y, c) = Halide::select(
            algo_id == 0, ahd_builder.output(x, y, c),        // if 0, use AHD
            algo_id == 1, lmmse_builder.output(x, y, c),     // if 1, use LMMSE
            algo_id == 2, ri_builder.output(x, y, c),        // if 2, use RI
                          fast_builder.output(x, y, c)       // else, use Fast
        );

        // --- Collect all intermediates for the scheduler ---
        all_intermediates.insert(all_intermediates.end(), ahd_builder.intermediates.begin(), ahd_builder.intermediates.end());
        all_intermediates.insert(all_intermediates.end(), lmmse_builder.intermediates.begin(), lmmse_builder.intermediates.end());
        all_intermediates.insert(all_intermediates.end(), ri_builder.intermediates.begin(), ri_builder.intermediates.end());
        all_intermediates.insert(all_intermediates.end(), fast_builder.intermediates.begin(), fast_builder.intermediates.end());
    }
};

#endif // STAGE_DEMOSAIC_H

