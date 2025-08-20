#ifndef STAGE_COLOR_CORRECT_H
#define STAGE_COLOR_CORRECT_H

#include "Halide.h"
#include <type_traits>
#include <vector>

template <typename T>
class ColorCorrectBuilder_T {
public:
    Halide::Func output;
    Halide::Func cc_matrix;
    std::vector<Halide::Func> intermediates;

    ColorCorrectBuilder_T(Halide::Func input,
                          Halide::Type proc_type,
                          Halide::Func matrix_3200,
                          Halide::Func matrix_7000,
                          Halide::Expr color_temp,
                          Halide::Expr tint,
                          Halide::Var x, Halide::Var y, Halide::Var c) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        output = Func("corrected");
        cc_matrix = Func("cc_matrix");

#ifdef NO_COLOR_CORRECT
        output(x, y, c) = input(x, y, c);
        cc_matrix(x,y) = 0.f; // Dummy
#else
        // The input is assumed to be normalized [0,1] and black-subtracted.
        // The color matrix is also assumed to have a normalized offset column.
        Expr ir_norm = input(x, y, 0);
        Expr ig_norm = input(x, y, 1);
        Expr ib_norm = input(x, y, 2);

        Var mx("cc_mx"), my("cc_my"); // Matrix coordinates
        Expr alpha = (1.0f / color_temp - 1.0f / 3200) / (1.0f / 7000 - 1.0f / 3200);
        cc_matrix(mx, my) = lerp(matrix_7000(mx, my), matrix_3200(mx, my), alpha);
        intermediates.push_back(cc_matrix);

        // Apply the 3x4 matrix (3x3 rotation + 1x3 offset) to the normalized,
        // black-subtracted sensor values. The result is linear sRGB.
        Expr r_f_srgb = cc_matrix(3, 0) + cc_matrix(0, 0) * ir_norm + cc_matrix(1, 0) * ig_norm + cc_matrix(2, 0) * ib_norm;
        Expr g_f_srgb = cc_matrix(3, 1) + cc_matrix(0, 1) * ir_norm + cc_matrix(1, 1) * ig_norm + cc_matrix(2, 1) * ib_norm;
        Expr b_f_srgb = cc_matrix(3, 2) + cc_matrix(0, 2) * ir_norm + cc_matrix(1, 2) * ig_norm + cc_matrix(2, 2) * ib_norm;

        // Apply tint in linear sRGB space.
        g_f_srgb = g_f_srgb * (1.0f - tint);

        Expr result_f = mux(c, {r_f_srgb, g_f_srgb, b_f_srgb});

        if (std::is_same<T, float>::value) {
            output(x, y, c) = clamp(result_f, 0.0f, 1.0f);
        } else {
            output(x, y, c) = u16_sat(clamp(result_f, 0.0f, 1.0f) * 65535.0f);
        }
#endif
    }
};

#endif // STAGE_COLOR_CORRECT_H

