#ifndef STAGE_DEINTERLEAVE_H
#define STAGE_DEINTERLEAVE_H

#include "Halide.h"

inline Halide::Func pipeline_deinterleave(Halide::Func raw, Halide::Var x, Halide::Var y, Halide::Var c) {
#ifdef NO_DEINTERLEAVE
    Halide::Func deinterleaved("deinterleaved_dummy");
    // Dummy stage: A real deinterleave changes shape from 2D to 3D (w/2, h/2, 4).
    // This dummy preserves the shape change but just puts the top-left
    // pixel of the 2x2 bayer quad into all 4 channels.
    deinterleaved(x, y, c) = raw(2 * x, 2 * y);
    return deinterleaved;
#else
    Halide::Func deinterleaved("deinterleaved");
    // Deinterleave the color channels based on the sensor's GRBG bayer pattern.
    // The output dimensions will be (width/2, height/2, 4).
    // The type of the input 'raw' Func is passed through to the output.
    deinterleaved(x, y, c) = Halide::mux(c,
                                 {raw(2 * x, 2 * y),       // G in a red row (G_r)
                                  raw(2 * x + 1, 2 * y),   // R
                                  raw(2 * x, 2 * y + 1),   // B
                                  raw(2 * x + 1, 2 * y + 1) // G in a blue row (G_b)
                                 });
    return deinterleaved;
#endif
}

#endif // STAGE_DEINTERLEAVE_H
