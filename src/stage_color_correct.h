#ifndef STAGE_COLOR_CORRECT_H
#define STAGE_COLOR_CORRECT_H

#include "Halide.h"

inline Halide::Func pipeline_color_correct(Halide::Func input,
                                           Halide::Func matrix_3200,
                                           Halide::Func matrix_7000,
                                           Halide::Expr color_temp,
                                           Halide::Expr tint,
                                           Halide::Var x, Halide::Var y, Halide::Var c,
                                           const Halide::Target &target,
                                           bool is_autoscheduled) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_COLOR_CORRECT
    Func corrected("corrected_dummy");
    // Dummy pass-through stage
    corrected(x, y, c) = input(x, y, c);
    return corrected;
#else
    Func corrected("corrected");
    // Get a color matrix by linearly interpolating between two
    // calibrated matrices using inverse kelvin.
    Func matrix("cc_matrix");
    Var mx("cc_mx"), my("cc_my"); // Matrix coordinates
    Expr alpha = (1.0f / color_temp - 1.0f / 3200) / (1.0f / 7000 - 1.0f / 3200);
    Expr val = (matrix_3200(mx, my) * alpha + matrix_7000(mx, my) * (1 - alpha));
    matrix(mx, my) = cast<int16_t>(val * 256.0f);  // Q8.8 fixed point

    if (!is_autoscheduled) {
        matrix.compute_root();
        if (target.has_gpu_feature()) {
            matrix.gpu_single_thread();
        }
    }

    Expr ir = cast<int32_t>(input(x, y, 0));
    Expr ig = cast<int32_t>(input(x, y, 1));
    Expr ib = cast<int32_t>(input(x, y, 2));

    Expr r = matrix(3, 0) + matrix(0, 0) * ir + matrix(1, 0) * ig + matrix(2, 0) * ib;
    Expr g = matrix(3, 1) + matrix(0, 1) * ir + matrix(1, 1) * ig + matrix(2, 1) * ib;
    Expr b = matrix(3, 2) + matrix(0, 2) * ir + matrix(1, 2) * ig + matrix(2, 2) * ib;

    // Apply tint adjustment to the green channel.
    // A positive tint value shifts the image towards magenta (by reducing green).
    // A negative tint value shifts the image towards green (by increasing green).
    g = cast<int32_t>(cast<float>(g) * (1.0f - tint));

    r = cast<int16_t>(r / 256);
    g = cast<int16_t>(g / 256);
    b = cast<int16_t>(b / 256);
    corrected(x, y, c) = mux(c, {r, g, b});
    return corrected;
#endif
}

#endif // STAGE_COLOR_CORRECT_H
