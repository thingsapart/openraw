#ifndef STAGE_HOT_PIXEL_SUPPRESSION_H
#define STAGE_HOT_PIXEL_SUPPRESSION_H

#include "Halide.h"

inline Halide::Func pipeline_hot_pixel_suppression(Halide::Func input, Halide::Var x, Halide::Var y) {
#ifdef NO_HOT_PIXEL_SUPPRESSION
    Halide::Func hps("hot_pixel_suppressed_dummy");
    // Dummy pass-through stage
    hps(x, y) = input(x, y);
    return hps;
#else
    Halide::Func hps("hot_pixel_suppressed");
    // Works on any numeric type thanks to Halide's generic max/clamp
    Halide::Expr max_neighbor = max(input(x - 2, y), input(x + 2, y),
                                  input(x, y - 2), input(x, y + 2));

    hps(x, y) = Halide::clamp(input(x, y), 0, max_neighbor);
    return hps;
#endif
}

#endif // STAGE_HOT_PIXEL_SUPPRESSION_H
