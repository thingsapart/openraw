#ifndef STAGE_COLOR_CORRECT_H
#define STAGE_COLOR_CORRECT_H

#include "Halide.h"
#include <type_traits>
#include <vector>

template <typename T>
class ColorCorrectBuilder_T {
public:
    Halide::Func output;
    Halide::Func cc_matrix; // The interpolated color correction matrix, now a public member.
    std::vector<Halide::Func> intermediates;

    ColorCorrectBuilder_T(Halide::Func input,
                          Halide::Type proc_type,
                          Halide::Func matrix_3200,
                          Halide::Func matrix_7000,
                          Halide::Expr color_temp,
                          Halide::Expr tint,
                          Halide::Var x, Halide::Var y, Halide::Var c,
                          Halide::Expr whiteLevel, Halide::Expr blackLevel) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        output = Func("corrected");

#ifdef NO_COLOR_CORRECT
        output(x, y, c) = input(x, y, c);
        // Provide a dummy matrix if the stage is disabled.
        cc_matrix = Func("cc_matrix_dummy");
        cc_matrix(x,y) = 0.f;
#else
        Expr ir = input(x, y, 0);
        Expr ig = input(x, y, 1);
        Expr ib = input(x, y, 2);

        // Get a color matrix by linearly interpolating between two
        // calibrated matrices using inverse kelvin.
        cc_matrix = Func("cc_matrix");
        Var mx("cc_mx"), my("cc_my"); // Matrix coordinates
        Expr alpha = (1.0f / color_temp - 1.0f / 3200) / (1.0f / 7000 - 1.0f / 3200);
        cc_matrix(mx, my) = lerp(matrix_7000(mx, my), matrix_3200(mx, my), alpha);
        // The matrix is an intermediate, but we don't add it to the vector
        // because we expose it directly for the forked pipeline.

        if (proc_type == Halide::Float(32)) {
            // --- Float Path ---
            // The matrix offset is in the integer domain. It must be normalized
            // to the [0,1] float domain before being applied to the float data.
            Expr range = cast<float>(whiteLevel - blackLevel);
            Expr r_f = cc_matrix(3, 0) / range + cc_matrix(0, 0) * ir + cc_matrix(1, 0) * ig + cc_matrix(2, 0) * ib;
            Expr g_f = cc_matrix(3, 1) / range + cc_matrix(0, 1) * ir + cc_matrix(1, 1) * ig + cc_matrix(2, 1) * ib;
            Expr b_f = cc_matrix(3, 2) / range + cc_matrix(0, 2) * ir + cc_matrix(1, 2) * ig + cc_matrix(2, 2) * ib;
            
            // Apply tint adjustment to the green channel.
            g_f = g_f * (1.0f - tint);
            
            output(x, y, c) = mux(c, {r_f, g_f, b_f});
            
        } else {
            // --- Integer Path ---
            // Use Q8.8 fixed point for matrix coefficients
            Func matrix_fixed("cc_matrix_fixed");
            matrix_fixed(mx, my) = cast<int16_t>(cc_matrix(mx, my) * 256.0f);
            intermediates.push_back(matrix_fixed);

            Expr ir_i32 = cast<int32_t>(ir);
            Expr ig_i32 = cast<int32_t>(ig);
            Expr ib_i32 = cast<int32_t>(ib);

            Expr r_i32 = (matrix_fixed(3, 0) + matrix_fixed(0, 0) * ir_i32 + matrix_fixed(1, 0) * ig_i32 + matrix_fixed(2, 0) * ib_i32) / 256;
            Expr g_i32 = (matrix_fixed(3, 1) + matrix_fixed(0, 1) * ir_i32 + matrix_fixed(1, 1) * ig_i32 + matrix_fixed(2, 1) * ib_i32) / 256;
            Expr b_i32 = (matrix_fixed(3, 2) + matrix_fixed(0, 2) * ir_i32 + matrix_fixed(1, 2) * ig_i32 + matrix_fixed(2, 2) * ib_i32) / 256;

            Expr r_f = cast<float>(r_i32);
            Expr g_f = cast<float>(g_i32) * (1.0f - tint);
            Expr b_f = cast<float>(b_i32);

            Expr result_f = mux(c, {r_f, g_f, b_f});
            output(x, y, c) = cast<uint16_t>(clamp(result_f, 0, 65535));
        }
#endif
    }
};

#endif // STAGE_COLOR_CORRECT_H
