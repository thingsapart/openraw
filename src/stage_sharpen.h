#ifndef STAGE_SHARPEN_H
#define STAGE_SHARPEN_H

#include "Halide.h"
#include "halide_trace_config.h"
#include "pipeline_helpers.h"

// OPTIMIZATION: This stage now operates on Float(32) data directly
// in linear space, before tonemapping. This improves quality and simplifies the math.
inline Halide::Func pipeline_sharpen(Halide::Func input_float,
                                     Halide::Expr sharpen_strength,
                                     Halide::Var x, Halide::Var y, Halide::Var c,
                                     Halide::Func &unsharp, Halide::Func &unsharp_y) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_SHARPEN
    Func sharpened("sharpened_dummy");
    // Dummy pass-through stage
    sharpened(x, y, c) = input_float(x, y, c);
    return sharpened;
#else
    // OPTIMIZATION: The intermediate blur passes are now exposed via references
    // so they can be explicitly scheduled in the main generator file.
    unsharp_y(x, y, c) = blur121_f(input_float(x, y - 1, c), input_float(x, y, c), input_float(x, y + 1, c));
    unsharp(x, y, c) = blur121_f(unsharp_y(x - 1, y, c), unsharp_y(x, y, c), unsharp_y(x + 1, y, c));

    Func mask("mask");
    mask(x, y, c) = input_float(x, y, c) - unsharp(x, y, c);

    // OPTIMIZATION: Removed fixed-point math. `sharpen_strength` is now used directly as a float.
    Func sharpened("sharpened");
    sharpened(x, y, c) = input_float(x, y, c) + mask(x, y, c) * sharpen_strength;
    return sharpened;
#endif
}

#endif // STAGE_SHARPEN_H
