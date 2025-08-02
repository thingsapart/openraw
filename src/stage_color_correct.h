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
    Func matrix("cc_matrix"); // **FIX:** Use the simple constructor. Type is inferred.
    Var mx("cc_mx"), my("cc_my"); // Matrix coordinates
    Expr alpha = (1.0f / color_temp - 1.0f / 3200) / (1.0f / 7000 - 1.0f / 3200);
    // The expression below is float, so Halide will infer that 'matrix' is a Float(32) Func.
    matrix(mx, my) = (matrix_3200(mx, my) * alpha + matrix_7000(mx, my) * (1 - alpha));

    if (!is_autoscheduled) {
        matrix.compute_root();
        if (target.has_gpu_feature()) {
            matrix.gpu_single_thread();
        }
    }

    // Perform the matrix multiplication. The input is already Float(32).
    Expr ir = input(x, y, 0);
    Expr ig = input(x, y, 1);
    Expr ib = input(x, y, 2);

    Expr r_f = matrix(3, 0) + matrix(0, 0) * ir + matrix(1, 0) * ig + matrix(2, 0) * ib;
    Expr g_f = matrix(3, 1) + matrix(0, 1) * ir + matrix(1, 1) * ig + matrix(2, 1) * ib;
    Expr b_f = matrix(3, 2) + matrix(0, 2) * ir + matrix(1, 2) * ig + matrix(2, 2) * ib;

    // Apply tint adjustment to the green channel.
    g_f = g_f * (1.0f - tint);

    // Output is Float(32) for the next stage. Clamping is deferred until we cast back to uint16.
    corrected(x, y, c) = mux(c, {r_f, g_f, b_f});
    return corrected;
#endif
}

#endif // STAGE_COLOR_CORRECT_H
