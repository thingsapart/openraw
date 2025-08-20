#ifndef STAGE_DEINTERLEAVE_H
#define STAGE_DEINTERLEAVE_H

#include "Halide.h"

// This stage is now simplified. It assumes its input is already in a
// canonical GRBG format. Its only job is to change the data layout from
// a 2D Bayer mosaic into a 3D Func with 4 channels representing the 2x2 quad.
inline Halide::Func pipeline_deinterleave(Halide::Func raw_grbg, Halide::Var x, Halide::Var y, Halide::Var c) {
#ifdef NO_DEINTERLEAVE
    Halide::Func deinterleaved("deinterleaved_dummy");
    deinterleaved(x, y, c) = raw_grbg(2 * x, 2 * y);
    return deinterleaved;
#else
    using namespace Halide;
    Func deinterleaved("deinterleaved");

    // The input is guaranteed to be in GRBG format.
    // G R
    // B G

    Expr gr = raw_grbg(2 * x,     2 * y);     // Top-left is G_r
    Expr r  = raw_grbg(2 * x + 1, 2 * y);     // Top-right is R
    Expr b  = raw_grbg(2 * x,     2 * y + 1); // Bottom-left is B
    Expr gb = raw_grbg(2 * x + 1, 2 * y + 1); // Bottom-right is G_b

    // Standardized Output channels: 0=G_r, 1=R, 2=B, 3=G_b
    deinterleaved(x, y, c) = mux(c, {gr, r, b, gb});

    return deinterleaved;
#endif
}

#endif // STAGE_DEINTERLEAVE_H
