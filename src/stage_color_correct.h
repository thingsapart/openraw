#ifndef STAGE_COLOR_CORRECT_H
#define STAGE_COLOR_CORRECT_H

#include "Halide.h"

inline Halide::Func pipeline_color_correct(Halide::Func input,
                                           Halide::Func matrix_3200,
                                           Halide::Func matrix_7000,
                                           Halide::Expr color_temp,
                                           Halide::Expr tint,
                                           Halide::Expr white_point,
                                           Halide::Var x, Halide::Var y, Halide::Var c,
                                           const Halide::Target &target,
                                           bool is_autoscheduled) {
    using namespace Halide;
    using namespace Halide::ConciseCasts;

#ifdef NO_COLOR_CORRECT
    Func corrected("corrected_dummy");
    corrected(x, y, c) = input(x, y, c);
    return corrected;
#else
    Func corrected("corrected");
    Func matrix("cc_matrix");
    Var mx("cc_mx"), my("cc_my");
    Expr alpha = (1.0f / color_temp - 1.0f / 3200) / (1.0f / 7000 - 1.0f / 3200);
    matrix(mx, my) = (matrix_3200(mx, my) * alpha + matrix_7000(mx, my) * (1 - alpha));

    if (!is_autoscheduled) {
        matrix.compute_root();
        if (target.has_gpu_feature()) {
            matrix.gpu_single_thread();
        }
    }

    // The input is normalized floating point data [0, N]. The color matrices were
    // calibrated for non-normalized data. To apply the matrix correctly, the color
    // components must be multiplied by their coefficients, and the offset must be
    // normalized by the white point before being added.
    Expr ir = input(x, y, 0);
    Expr ig = input(x, y, 1);
    Expr ib = input(x, y, 2);

    Expr r_f = matrix(3, 0) / white_point + matrix(0, 0) * ir + matrix(1, 0) * ig + matrix(2, 0) * ib;
    Expr g_f = matrix(3, 1) / white_point + matrix(0, 1) * ir + matrix(1, 1) * ig + matrix(2, 1) * ib;
    Expr b_f = matrix(3, 2) / white_point + matrix(0, 2) * ir + matrix(1, 2) * ig + matrix(2, 2) * ib;

    g_f = g_f * (1.0f - tint);

    corrected(x, y, c) = mux(c, {r_f, g_f, b_f});
    return corrected;
#endif
}

#endif // STAGE_COLOR_CORRECT_H
