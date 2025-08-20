#ifndef STAGE_DEHAZE_H
#define STAGE_DEHAZE_H

#include "Halide.h"
#include "pipeline_helpers.h"

class DehazeBuilder {
public:
    Halide::Func output;

    DehazeBuilder(Halide::Func input_srgb, Halide::Expr strength, Halide::Var x, Halide::Var y, Halide::Var c)
        : output("dehazed")
    {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        // --- Dehaze using Color Attenuation Prior ---
        Expr r = input_srgb(x, y, 0);
        Expr g = input_srgb(x, y, 1);
        Expr b = input_srgb(x, y, 2);

        // Brightness and Saturation are used to estimate haze depth.
        Expr v = max(r, g, b);
        Expr s = (v - min(r, g, b)) / (v + 1e-6f);
        Expr d = v - s; // Depth estimate

        // Transmission = 1 - strength * depth. Clamp to avoid over-dehazing.
        Expr t = clamp(1.0f - (strength / 100.0f) * d, 0.1f, 1.0f);

        // Assume atmospheric light A is pure white (1,1,1) for speed.
        const float A = 1.0f;

        // Invert the haze model: J = (I - A)/t + A
        Expr val_dehazed = (input_srgb(x, y, c) - A) / t + A;

        // If dehaze is disabled, pass through. Otherwise, apply the dehazing and
        // clamp the result to be non-negative to prevent numerical errors in subsequent
        // color space conversions.
        output(x, y, c) = select(strength < 0.001f,
                                 input_srgb(x, y, c),
                                 max(0.0f, val_dehazed));
    }
};

#endif // STAGE_DEHAZE_H

