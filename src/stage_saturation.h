#ifndef STAGE_SATURATION_H
#define STAGE_SATURATION_H

#include "Halide.h"

inline Halide::Func pipeline_saturation(Halide::Func input,
                                        Halide::Expr saturation,
                                        Halide::Var x, Halide::Var y, Halide::Var c) {
#ifdef NO_SATURATION
    Halide::Func saturated("saturated_dummy");
    // Dummy pass-through stage
    saturated(x, y, c) = input(x, y, c);
    return saturated;
#else
    using namespace Halide;
    using namespace Halide::ConciseCasts;

    Func saturated("saturated");

    // Get the R, G, B components. They are int16.
    Expr r = input(x, y, 0);
    Expr g = input(x, y, 1);
    Expr b = input(x, y, 2);

    // Calculate luminance. Use float for precision.
    // The coefficients are for sRGB and are a good standard.
    Expr r_f = cast<float>(r);
    Expr g_f = cast<float>(g);
    Expr b_f = cast<float>(b);
    Expr luma = 0.299f * r_f + 0.587f * g_f + 0.114f * b_f;

    // Interpolate between the luma and the original color
    Expr r_new = luma + saturation * (r_f - luma);
    Expr g_new = luma + saturation * (g_f - luma);
    Expr b_new = luma + saturation * (b_f - luma);

    // Create the new pixel and clamp back to int16 range.
    Expr r_sat = cast<int16_t>(clamp(r_new, 0.0f, 32767.0f));
    Expr g_sat = cast<int16_t>(clamp(g_new, 0.0f, 32767.0f));
    Expr b_sat = cast<int16_t>(clamp(b_new, 0.0f, 32767.0f));

    saturated(x, y, c) = mux(c, {r_sat, g_sat, b_sat});

    return saturated;
#endif
}

#endif // STAGE_SATURATION_H
