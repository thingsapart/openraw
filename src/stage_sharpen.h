#ifndef STAGE_SHARPEN_H
#define STAGE_SHARPEN_H

#include "Halide.h"
#include "halide_trace_config.h"
#include "pipeline_helpers.h"

// **FIX:** This stage now operates on 16-bit data.
inline Halide::Func pipeline_sharpen(Halide::Func input,
                                     Halide::Expr sharpen_strength,
                                     Halide::Var x, Halide::Var y, Halide::Var c,
                                     const Halide::Target &target,
                                     bool is_autoscheduled) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_SHARPEN
    Func sharpened("sharpened_dummy");
    // Dummy pass-through stage
    sharpened(x, y, c) = input(x, y, c);
    return sharpened;
#else
    Func sharpened("sharpened");

    // The input is uint16. For sharpening, it's safer and cleaner to work
    // with floats to avoid intermediate clipping and overflow.
    Func input_f("sharpen_input_f");
    input_f(x, y, c) = cast<float>(input(x, y, c));

    // A simple 3x3 box blur in floating point.
    Func blur_x("sharpen_blur_x");
    blur_x(x, y, c) = (input_f(x - 1, y, c) + input_f(x, y, c) + input_f(x + 1, y, c)) / 3.0f;

    Func unsharp("unsharp");
    unsharp(x, y, c) = (blur_x(x, y - 1, c) + blur_x(x, y, c) + blur_x(x, y + 1, c)) / 3.0f;

    // The mask is the difference between the original and the blurred image.
    Expr mask = input_f(x, y, c) - unsharp(x, y, c);

    // Apply the sharpened mask. The sharpen_strength is a float.
    Expr sharpened_f = input_f(x, y, c) + mask * sharpen_strength;

    // Clamp the result back to the 16-bit range and cast to the final type.
    sharpened(x, y, c) = cast<uint16_t>(clamp(sharpened_f, 0.0f, 65535.0f));

    return sharpened;
#endif
}

#endif // STAGE_SHARPEN_H
