#ifndef PIPELINE_HELPERS_H
#define PIPELINE_HELPERS_H

#include "Halide.h"
#include <type_traits>

// A helper to saturate to the processing type's range.
// For uint16_t, this is 0-65535.
// For float, this is 0.0-1.0.
template <typename T>
inline Halide::Expr proc_type_sat(Halide::Expr val) {
    using namespace Halide::ConciseCasts;
    if (std::is_same<T, uint16_t>::value) {
        return u16_sat(val);
    } else {
        return Halide::clamp(val, 0.0f, 1.0f);
    }
}

// Average two positive values, aware of integer vs float types.
inline Halide::Expr avg(Halide::Expr a, Halide::Expr b) {
    if (a.type().is_int() || a.type().is_uint()) {
        Halide::Type wider = a.type().with_bits(a.type().bits() * 2);
        return Halide::cast(a.type(), (Halide::cast(wider, a) + b + 1) / 2);
    } else {
        return (a + b) / 2.0f;
    }
}

// Average four positive values, aware of integer vs float types.
inline Halide::Expr avg(Halide::Expr a, Halide::Expr b, Halide::Expr c, Halide::Expr d) {
    if (a.type().is_int() || a.type().is_uint()) {
        Halide::Type wider = a.type().with_bits(a.type().bits() * 2);
        return Halide::cast(a.type(), (Halide::cast(wider, a) + b + c + d + 2) / 4);
    } else {
        return (a + b + c + d) / 4.0f;
    }
}

// 1-2-1 blur, which automatically becomes type-aware via avg().
inline Halide::Expr blur121(Halide::Expr a, Halide::Expr b, Halide::Expr c) {
    return avg(avg(a, c), b);
}

#endif // PIPELINE_HELPERS_H
