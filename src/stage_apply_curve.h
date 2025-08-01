#ifndef STAGE_APPLY_CURVE_H
#define STAGE_APPLY_CURVE_H

#include "Halide.h"
#include "halide_trace_config.h"

inline Halide::Func pipeline_apply_curve(Halide::Func input,
                                         Halide::Expr blackLevel,
                                         Halide::Expr whiteLevel,
                                         Halide::Expr contrast,
                                         Halide::Expr gamma,
                                         Halide::Var x, Halide::Var y, Halide::Var c,
                                         Halide::Type result_type,
                                         const Halide::Target &target,
                                         bool is_autoscheduled) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_APPLY_CURVE
    Func curved("curved_dummy");
    // Dummy stage: a simple bit-shift to convert 16-bit data to 8-bit.
    // This is critical because the next stage expects 8-bit input.
    curved(x, y, c) = cast(result_type, clamp(input(x, y, c), 0, 16383) >> 8);
    return curved;
#else
    Func curved("curved");
    Func tone_curve("tone_curve");

    Expr minRaw = 0 + blackLevel;
    Expr maxRaw = whiteLevel;

    int lutResample = 1;
    if (target.has_feature(Target::HVX)) {
        lutResample = 8;
    }

    minRaw /= lutResample;
    maxRaw /= lutResample;

    Expr invRange = 1.0f / (maxRaw - minRaw);
    Expr b = 2.0f - pow(2.0f, contrast / 100.0f);
    Expr a = 2.0f - 2.0f * b;

    // Use a different var for the LUT definition to avoid confusion
    Var lut_x("lut_x");
    Expr xf = clamp(cast<float>(lut_x - minRaw) * invRange, 0.0f, 1.0f);
    Expr g = pow(xf, 1.0f / gamma);
    Expr z = select(g > 0.5f,
                    1.0f - (a * (1.0f - g) * (1.0f - g) + b * (1.0f - g)),
                    a * g * g + b * g);

    Expr val = cast(result_type, clamp(z * 255.0f + 0.5f, 0.0f, 255.0f));
    tone_curve(lut_x) = select(lut_x <= minRaw, 0, select(lut_x > maxRaw, 255, val));

    if (!is_autoscheduled) {
        tone_curve.compute_root();
        if (target.has_gpu_feature()) {
            Var xi;
            tone_curve.gpu_tile(lut_x, xi, 32);
        }
    }

    Halide::Trace::FuncConfig cfg;
    cfg.labels = {{"tone curve"}};
    cfg.pos = {580, 1000};
    tone_curve.add_trace_tag(cfg.to_trace_tag());

    if (lutResample == 1) {
        curved(x, y, c) = tone_curve(clamp(input(x, y, c), 0, 1023));
    } else {
        Expr in = input(x, y, c);
        Expr u0 = in / lutResample;
        Expr u = in % lutResample;
        Expr y0 = tone_curve(clamp(u0, 0, 127));
        Expr y1 = tone_curve(clamp(u0 + 1, 0, 127));
        curved(x, y, c) = cast<uint8_t>((cast<uint16_t>(y0) * lutResample + (y1 - y0) * u) / lutResample);
    }
    return curved;
#endif
}

#endif // STAGE_APPLY_CURVE_H
