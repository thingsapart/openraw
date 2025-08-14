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
                          Halide::Var x, Halide::Var y, Halide::Var c,
                          Halide::Expr whiteLevel, Halide::Expr blackLevel) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        output = Func("corrected");
        cc_matrix = Func("cc_matrix");

#ifdef NO_COLOR_CORRECT
        output(x, y, c) = input(x, y, c);
        cc_matrix(x,y) = 0.f; // Dummy
#else
        Expr ir_sensor, ig_sensor, ib_sensor;
        Expr range = cast<float>(whiteLevel - blackLevel);

        // The matrix operates on sensor-range data. Ensure the input is in that range.
        if (proc_type == Halide::Float(32)) {
            // The f32 pipeline's input is normalized [0,1]. Scale it back to sensor range.
            ir_sensor = input(x, y, 0) * range + blackLevel;
            ig_sensor = input(x, y, 1) * range + blackLevel;
            ib_sensor = input(x, y, 2) * range + blackLevel;
        } else {
            // The u16 pipeline's input is already in sensor range. Just cast.
            ir_sensor = cast<float>(input(x, y, 0));
            ig_sensor = cast<float>(input(x, y, 1));
            ib_sensor = cast<float>(input(x, y, 2));
        }

        Var mx("cc_mx"), my("cc_my"); // Matrix coordinates
        Expr alpha = (1.0f / color_temp - 1.0f / 3200) / (1.0f / 7000 - 1.0f / 3200);
        cc_matrix(mx, my) = lerp(matrix_7000(mx, my), matrix_3200(mx, my), alpha);
        intermediates.push_back(cc_matrix);

        // Apply the matrix to sensor-range float data
        Expr r_f_sensor = cc_matrix(3, 0) + cc_matrix(0, 0) * ir_sensor + cc_matrix(1, 0) * ig_sensor + cc_matrix(2, 0) * ib_sensor;
        Expr g_f_sensor = cc_matrix(3, 1) + cc_matrix(0, 1) * ir_sensor + cc_matrix(1, 1) * ig_sensor + cc_matrix(2, 1) * ib_sensor;
        Expr b_f_sensor = cc_matrix(3, 2) + cc_matrix(0, 2) * ir_sensor + cc_matrix(1, 2) * ig_sensor + cc_matrix(2, 2) * ib_sensor;
        
        // Apply tint in the same space
        g_f_sensor = g_f_sensor * (1.0f - tint);
        
        // Normalize the result to [0,1]
        Expr inv_range = 1.0f / range;
        Expr r_f_norm = (r_f_sensor - blackLevel) * inv_range;
        Expr g_f_norm = (g_f_sensor - blackLevel) * inv_range;
        Expr b_f_norm = (b_f_sensor - blackLevel) * inv_range;

        Expr result_f = mux(c, {r_f_norm, g_f_norm, b_f_norm});

        if (std::is_same<T, float>::value) {
            output(x, y, c) = clamp(result_f, 0.0f, 1.0f);
        } else {
            output(x, y, c) = u16_sat(clamp(result_f, 0.0f, 1.0f) * 65535.0f);
        }
#endif
    }
};

#endif // STAGE_COLOR_CORRECT_H
