#ifndef PIPELINE_HELPERS_H
#define PIPELINE_HELPERS_H

#include "Halide.h"

// Average two positive values rounding up
inline Halide::Expr avg(Halide::Expr a, Halide::Expr b) {
    Halide::Type wider = a.type().with_bits(a.type().bits() * 2);
    return Halide::cast(a.type(), (Halide::cast(wider, a) + b + 1) / 2);
}

// 1-2-1 blur
inline Halide::Expr blur121(Halide::Expr a, Halide::Expr b, Halide::Expr c) {
    return avg(avg(a, c), b);
}

#endif // PIPELINE_HELPERS_H
