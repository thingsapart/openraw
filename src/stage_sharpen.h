#ifndef STAGE_SHARPEN_H
#define STAGE_SHARPEN_H

#include "Halide.h"
#include "halide_trace_config.h"
#include "pipeline_helpers.h"

inline Halide::Func pipeline_sharpen(Halide::Func input,
                                     Halide::Expr sharpen_strength,
                                     Halide::Var x, Halide::Var y, Halide::Var c,
                                     const Halide::Target &target,
                                     bool is_autoscheduled) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_SHARPEN
    Func sharpened("sharpened_dummy");
    // Dummy pass-through stage
    sharpened(x, y, c) = input(x, y, c);
    return sharpened;
#else
    Func sharpened("sharpened");

    Func sharpen_strength_x32("sharpen_strength_x32");
    sharpen_strength_x32() = u8_sat(sharpen_strength * 32);
    if (!is_autoscheduled) {
        sharpen_strength_x32.compute_root();
        if (target.has_gpu_feature()) {
            sharpen_strength_x32.gpu_single_thread();
        }
    }

    Halide::Trace::FuncConfig cfg;
    cfg.labels = {{"sharpen strength"}};
    cfg.pos = {10, 1000};
    sharpen_strength_x32.add_trace_tag(cfg.to_trace_tag());

    Func unsharp_y("unsharp_y");
    unsharp_y(x, y, c) = blur121(input(x, y - 1, c), input(x, y, c), input(x, y + 1, c));

    Func unsharp("unsharp");
    unsharp(x, y, c) = blur121(unsharp_y(x - 1, y, c), unsharp_y(x, y, c), unsharp_y(x + 1, y, c));

    Func mask("mask");
    mask(x, y, c) = cast<int32_t>(input(x, y, c)) - cast<int32_t>(unsharp(x, y, c));

    sharpened(x, y, c) = u16_sat(cast<int32_t>(input(x, y, c)) + (mask(x, y, c) * sharpen_strength_x32()) / 32);
    return sharpened;
#endif
}

#endif // STAGE_SHARPEN_H
