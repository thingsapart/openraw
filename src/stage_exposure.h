#ifndef STAGE_EXPOSURE_H
#define STAGE_EXPOSURE_H

#include "Halide.h"

inline Halide::Func pipeline_exposure(Halide::Func input,
                                      Halide::Expr exposure,
                                      Halide::Var x, Halide::Var y, Halide::Var c) {
#ifdef NO_EXPOSURE
    Halide::Func exposed("exposed_dummy");
    // Dummy pass-through stage
    exposed(x, y, c) = input(x, y, c);
    return exposed;
#else
    using namespace Halide;
    using namespace Halide::ConciseCasts;

    Func exposed("exposed");

    // Multiply the linear data by the exposure factor.
    // Cast to float for the multiplication, then clamp to the valid range
    // of the 16-bit unsigned integer type and cast back.
    Expr val_f = cast<float>(input(x, y, c)) * exposure;
    exposed(x, y, c) = cast<uint16_t>(clamp(val_f, 0.0f, 65535.0f));
    return exposed;
#endif
}

#endif // STAGE_EXPOSURE_H
