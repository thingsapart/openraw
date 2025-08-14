#ifndef STAGE_APPLY_CURVE_H
#define STAGE_APPLY_CURVE_H

#include "Halide.h"
#include "halide_trace_config.h"
#include "pipeline_helpers.h"
#include <type_traits>

template <typename T>
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
    curved(x, y, c) = proc_type_sat<T>(input(x, y, c));
    return curved;
#else
    Func curved("curved");
    
    Expr val = input(x, y, c);
    Expr norm_val;

    if (std::is_same<T, float>::value) {
        // For the float path, the value is already normalized to [0, 1].
        norm_val = val;
    } else {
        // For the uint16 path, the data is in the full [0, 65535] range.
        // Normalize it to [0, 1] to index into the curve.
        norm_val = cast<float>(val) / 65535.0f;
    }

    // Use the normalized value to index into the LUT.
    Expr lut_f_idx = clamp(norm_val, 0.0f, 1.0f) * (lut_size - 1.0f);
    Expr lut_idx = cast<int>(lut_f_idx);
    
    Expr lut_val = lut(lut_idx, c);

    if (std::is_same<T, float>::value) {
        // For the float path, the LUT value (uint16) must be scaled back to [0, 1].
        curved(x, y, c) = cast<float>(lut_val) / 65535.0f;
    } else {
        // For the uint16 path, the LUT value is already in the correct format.
        curved(x, y, c) = lut_val;
    }

    if (!is_autoscheduled && target.has_gpu_feature()) {
        // The LUT is passed as an Input, so we just need to ensure it's copied to the GPU.
        // Halide handles this automatically. No scheduling for the LUT itself is needed here.
    }
    
    return curved;
#endif
}

#endif // STAGE_APPLY_CURVE_H
