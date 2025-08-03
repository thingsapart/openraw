#ifndef STAGE_APPLY_CURVE_H
#define STAGE_APPLY_CURVE_H

#include "Halide.h"
#include "halide_trace_config.h"

inline Halide::Func pipeline_apply_curve(Halide::Func input,
                                         Halide::Expr blackLevel,
                                         Halide::Expr whiteLevel,
                                         Halide::Func lut,
                                         Halide::Expr lut_size,
                                         Halide::Var x, Halide::Var y, Halide::Var c,
                                         const Halide::Target &target,
                                         bool is_autoscheduled) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_APPLY_CURVE
    Func curved("curved_dummy");
    // Dummy stage: pass-through int16 to uint16
    curved(x, y, c) = cast<uint16_t>(clamp(input(x, y, c), 0, 65535));
    return curved;
#else
    Func curved("curved");

    // This stage applies the pre-computed 16-bit tone curve LUT.
    // The input is int16 from the color correction stage.
    // The output is uint16.
    
    // Normalize the input value from the camera's range to [0, 1]
    Expr val = input(x, y, c);
    Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
    Expr norm_val = (cast<float>(val) - blackLevel) * inv_range;
    
    // Use the normalized value to index into the LUT.
    Expr lut_f_idx = clamp(norm_val, 0.0f, 1.0f) * (lut_size - 1.0f);
    Expr lut_idx = cast<int>(lut_f_idx);

    // Apply the per-channel LUT.
    // A 65536-entry LUT is high enough resolution that we can use
    // nearest-neighbor lookup instead of linear interpolation without
    // visible posterization artifacts.
    curved(x, y, c) = lut(lut_idx, c);

    if (!is_autoscheduled && target.has_gpu_feature()) {
        // The LUT is passed as an Input, so we just need to ensure it's copied to the GPU.
        // Halide handles this automatically. No scheduling for the LUT itself is needed here.
    }
    
    // No tracing for the curve itself, as it's now an input.
    // The visualization will be done by the C++ host code.

    return curved;
#endif
}

#endif // STAGE_APPLY_CURVE_H
