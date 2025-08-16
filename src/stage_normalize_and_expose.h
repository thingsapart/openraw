#ifndef STAGE_NORMALIZE_AND_EXPOSE_H
#define STAGE_NORMALIZE_AND_EXPOSE_H

#include "Halide.h"

// This new stage combines black level subtraction, white point scaling (normalization),
// and exposure compensation into a single, efficient linear operation.
// It is the first stage to operate on the raw data after basic denoising and CA correction.
// It converts the data from uint16 to float32 for the rest of the linear pipeline.
inline Halide::Func pipeline_normalize_and_expose(Halide::Func input_uint16,
                                                  Halide::Expr black_point,
                                                  Halide::Expr white_point,
                                                  Halide::Expr exposure,
                                                  Halide::Var x, Halide::Var y) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

    Func normalized("normalized_and_exposed");

    // Convert to float for processing.
    Expr val_f = cast<float>(input_uint16(x, y));

    // 1. Subtract the black point to establish a true zero.
    Expr val_zeroed = val_f - black_point;

    // 2. Calculate the scaling factor.
    // This combines normalization (to map the white point to 1.0) and exposure.
    Expr inv_white_level = 1.0f / (white_point - black_point);
    Expr scale_factor = inv_white_level * exposure;

    // 3. Apply the scaling factor.
    normalized(x, y) = val_zeroed * scale_factor;

    return normalized;
}

#endif // STAGE_NORMALIZE_AND_EXPOSE_H

