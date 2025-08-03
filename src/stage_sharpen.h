#ifndef STAGE_SHARPEN_H
#define STAGE_SHARPEN_H

#include "Halide.h"
#include "halide_trace_config.h"
#include "pipeline_helpers.h"
#include <type_traits>

template <typename T>
inline Halide::Func pipeline_sharpen(Halide::Func input,
                                     Halide::Expr sharpen_strength,
                                     Halide::Var x, Halide::Var y, Halide::Var c,
                                     const Halide::Target &target,
                                     bool is_autoscheduled) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_SHARPEN
    Func sharpened("sharpened_dummy");
    sharpened(x, y, c) = input(x, y, c);
    return sharpened;
#else
    Func sharpened("sharpened");

    // Create a blurred version of the image (unsharp mask)
    Func unsharp_y("unsharp_y");
    unsharp_y(x, y, c) = blur121(input(x, y - 1, c), input(x, y, c), input(x, y + 1, c));

    Func unsharp("unsharp");
    unsharp(x, y, c) = blur121(unsharp_y(x - 1, y, c), unsharp_y(x, y, c), unsharp_y(x + 1, y, c));

    // Calculate the sharpening mask (difference between original and blurred)
    // Use a wider signed type for the subtraction if the input is integer.
    Expr mask;
    if (std::is_same<T, uint16_t>::value) {
        mask = cast<int32_t>(input(x, y, c)) - cast<int32_t>(unsharp(x, y, c));
    } else {
        mask = input(x, y, c) - unsharp(x, y, c);
    }

    // Add the scaled mask back to the original image.
    Expr sharpened_val = input(x, y, c) + mask * sharpen_strength;
    
    sharpened(x, y, c) = proc_type_sat<T>(sharpened_val);
    
    return sharpened;
#endif
}

#endif // STAGE_SHARPEN_H
