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
    curved(x, y, c) = u8_sat(input(x, y, c) >> 8);
    return curved;
#else
    Func curved("curved");

    // The input to this stage is the output of the local adjustments, which is
    // a display-referred image in the full uint16 range [0, 65535].
    // The tone curve LUT is also defined over this [0, 65535] domain.
    // We can therefore use the input value directly as an index.
    Expr val = input(x, y, c);

    // The lut contains uint16 values representing the final 8-bit output
    // scaled up (e.g., value 255 is stored as 65535). We need to shift
    // it back down to the 8-bit range.
    Expr lut_val_u16 = lut(val, c);
    Expr final_val_u8 = cast<uint8_t>(lut_val_u16 >> 8);

    curved(x, y, c) = final_val_u8;

    return curved;
#endif // NO_APPLY_CURVE
}

#endif // STAGE_APPLY_CURVE_H
