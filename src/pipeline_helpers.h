#ifndef PIPELINE_HELPERS_H
#define PIPELINE_HELPERS_H

#include "Halide.h"

// Average two positive values rounding up
inline Halide::Expr avg(Halide::Expr a, Halide::Expr b) {
    Halide::Type wider = a.type().with_bits(a.type().bits() * 2);
    return Halide::cast(a.type(), (Halide::cast(wider, a) + b + 1) / 2);
}

// 1-2-1 blur for integer types
inline Halide::Expr blur121(Halide::Expr a, Halide::Expr b, Halide::Expr c) {
    return avg(avg(a, c), b);
}

// 1-2-1 blur for float types
inline Halide::Expr blur121_f(Halide::Expr a, Halide::Expr b, Halide::Expr c) {
    return (a + b + b + c) * 0.25f;
}


#endif // PIPELINE_HELPERS_H
