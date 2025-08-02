#ifndef STAGE_EXPOSURE_H
#define STAGE_EXPOSURE_H

#include "Halide.h"

// DEPRECATED: This stage is now combined into stage_normalize_and_expose.h
// The file is kept for reference but should not be included in the generator.

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

    // Multiply the linear float data by the exposure factor.
    // The input is already Float(32), so no casting is needed.
    exposed(x, y, c) = input(x, y, c) * exposure;
    return exposed;
#endif
}

#endif // STAGE_EXPOSURE_H
