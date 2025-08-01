#ifndef STAGE_HOT_PIXEL_SUPPRESSION_H
#define STAGE_HOT_PIXEL_SUPPRESSION_H

#include "Halide.h"

inline Halide::Func pipeline_hot_pixel_suppression(Halide::Func input, Halide::Var x, Halide::Var y) {
#ifdef NO_HOT_PIXEL_SUPPRESSION
    Halide::Func denoised("denoised_dummy");
    // Dummy pass-through stage
    denoised(x, y) = input(x, y);
    return denoised;
#else
    Halide::Func denoised("denoised");
    // Real implementation
    Halide::Expr max_neighbor = max(input(x - 2, y), input(x + 2, y),
                                  input(x, y - 2), input(x, y + 2));

    denoised(x, y) = Halide::clamp(input(x, y), 0, max_neighbor);
    return denoised;
#endif
}

#endif // STAGE_HOT_PIXEL_SUPPRESSION_H
