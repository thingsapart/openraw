#ifndef STAGE_COLOR_CORRECT_H
#define STAGE_COLOR_CORRECT_H

#include "Halide.h"
#include <type_traits>
#include <vector>

template <typename T>
class ColorCorrectBuilder_T {
public:
    Halide::Func output;
    Halide::Func cc_matrix; // Expose the matrix for the local laplacian's low-fi path.
    std::vector<Halide::Func> intermediates;

    ColorCorrectBuilder_T(Halide::Func input,
                          Halide::Type proc_type,
                          Halide::Func color_matrix,
                          Halide::Var x, Halide::Var y, Halide::Var c) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        output = Func("corrected");
        // Assign the input matrix Func to the member Func so it can be accessed by other stages.
        cc_matrix = color_matrix;

#ifdef NO_COLOR_CORRECT
        output(x, y, c) = input(x, y, c);
#else
        // The input is assumed to be white-balanced, normalized [0,1],
        // and black-subtracted sensor data.
        Expr r_sensor = input(x, y, 0);
        Expr g_sensor = input(x, y, 1);
        Expr b_sensor = input(x, y, 2);

        // Apply the D65 color matrix.
        // The matrix is 3x4, with the 4th column being a normalized offset.
        Expr r_srgb = color_matrix(3, 0) + color_matrix(0, 0) * r_sensor + color_matrix(1, 0) * g_sensor + color_matrix(2, 0) * b_sensor;
        Expr g_srgb = color_matrix(3, 1) + color_matrix(0, 1) * r_sensor + color_matrix(1, 1) * g_sensor + color_matrix(2, 1) * b_sensor;
        Expr b_srgb = color_matrix(3, 2) + color_matrix(0, 2) * r_sensor + color_matrix(1, 2) * g_sensor + color_matrix(2, 2) * b_sensor;

        Expr result_f = mux(c, {r_srgb, g_srgb, b_srgb});

        if (std::is_same<T, float>::value) {
            output(x, y, c) = clamp(result_f, 0.0f, 1.0f);
        } else {
            output(x, y, c) = u16_sat(clamp(result_f, 0.0f, 1.0f) * 65535.0f);
        }
#endif
    }
};

#endif // STAGE_COLOR_CORRECT_H

