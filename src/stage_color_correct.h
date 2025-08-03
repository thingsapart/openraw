#ifndef STAGE_COLOR_CORRECT_H
#define STAGE_COLOR_CORRECT_H

#include "Halide.h"
#include <type_traits>

inline Halide::Func pipeline_color_correct(Halide::Func input,
                                           Halide::Type proc_type,
                                           Halide::Func matrix_3200,
                                           Halide::Func matrix_7000,
                                           Halide::Expr color_temp,
                                           Halide::Expr tint,
                                           Halide::Var x, Halide::Var y, Halide::Var c,
                                           Halide::Expr whiteLevel, Halide::Expr blackLevel,
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

    Expr ir = input(x, y, 0);
    Expr ig = input(x, y, 1);
    Expr ib = input(x, y, 2);

    // Get a color matrix by linearly interpolating between two
    // calibrated matrices using inverse kelvin.
    Func matrix("cc_matrix");
    Var mx("cc_mx"), my("cc_my"); // Matrix coordinates
    Expr alpha = (1.0f / color_temp - 1.0f / 3200) / (1.0f / 7000 - 1.0f / 3200);
    matrix(mx, my) = lerp(matrix_7000(mx, my), matrix_3200(mx, my), alpha);
    
    if (!is_autoscheduled) {
        matrix.compute_root();
        if (target.has_gpu_feature()) {
            matrix.gpu_single_thread();
        }
    }

    if (proc_type == Halide::Float(32)) {
        // --- Float Path ---
        // The matrix offset is in the integer domain. It must be normalized
        // to the [0,1] float domain before being applied to the float data.
        Expr range = cast<float>(whiteLevel - blackLevel);
        Expr r_f = matrix(3, 0) / range + matrix(0, 0) * ir + matrix(1, 0) * ig + matrix(2, 0) * ib;
        Expr g_f = matrix(3, 1) / range + matrix(0, 1) * ir + matrix(1, 1) * ig + matrix(2, 1) * ib;
        Expr b_f = matrix(3, 2) / range + matrix(0, 2) * ir + matrix(1, 2) * ig + matrix(2, 2) * ib;
        
        // Apply tint adjustment to the green channel.
        g_f = g_f * (1.0f - tint);
        
        corrected(x, y, c) = mux(c, {r_f, g_f, b_f});
        
    } else {
        // --- Integer Path ---
        // Use Q8.8 fixed point for matrix coefficients
        Func matrix_fixed("cc_matrix_fixed");
        matrix_fixed(mx, my) = cast<int16_t>(matrix(mx, my) * 256.0f);
        if (!is_autoscheduled) {
            matrix_fixed.compute_at(corrected, c);
        }

        Expr ir_i32 = cast<int32_t>(ir);
        Expr ig_i32 = cast<int32_t>(ig);
        Expr ib_i32 = cast<int32_t>(ib);

        Expr r_i32 = (matrix_fixed(3, 0) + matrix_fixed(0, 0) * ir_i32 + matrix_fixed(1, 0) * ig_i32 + matrix_fixed(2, 0) * ib_i32) / 256;
        Expr g_i32 = (matrix_fixed(3, 1) + matrix_fixed(0, 1) * ir_i32 + matrix_fixed(1, 1) * ig_i32 + matrix_fixed(2, 1) * ib_i32) / 256;
        // BUG FIX: The middle term was ib_i32, now it is correctly ig_i32.
        Expr b_i32 = (matrix_fixed(3, 2) + matrix_fixed(0, 2) * ir_i32 + matrix_fixed(1, 2) * ig_i32 + matrix_fixed(2, 2) * ib_i32) / 256;

        Expr r_f = cast<float>(r_i32);
        Expr g_f = cast<float>(g_i32) * (1.0f - tint);
        Expr b_f = cast<float>(b_i32);

        Expr result_f = mux(c, {r_f, g_f, b_f});
        corrected(x, y, c) = cast<uint16_t>(clamp(result_f, 0, 65535));
    }
    return corrected;
#endif
}

#endif // STAGE_COLOR_CORRECT_H
