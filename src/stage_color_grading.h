#ifndef STAGE_COLOR_GRADING_H
#define STAGE_COLOR_GRADING_H

#include "Halide.h"

class ColorGradeBuilder {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;

    ColorGradeBuilder(Halide::Func lch_input, Halide::Func lut, Halide::Expr lut_dim_extent, Halide::Var x, Halide::Var y, Halide::Var c) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;
        output = Func("color_graded");

#ifndef NO_3D_LUTS
        Expr l = lch_input(x, y, 0);
        Expr C = lch_input(x, y, 1);
        Expr h = lch_input(x, y, 2); // Radians [-pi, pi]

        // Normalize coordinates for LUT lookup
        Expr l_norm = clamp(l / 100.f, 0.f, 1.f);
        Expr C_norm = clamp(C / 150.f, 0.f, 1.f);
        Expr h_norm = clamp((h + (float)M_PI) / (2.f * (float)M_PI), 0.f, 1.f); // [0, 1]

        Expr lut_dim = lut_dim_extent;
        Expr lf = l_norm * (lut_dim - 1);
        Expr Cf = C_norm * (lut_dim - 1);
        Expr hf = h_norm * (lut_dim - 1);

        Expr li = cast<int>(floor(lf));
        Expr Ci = cast<int>(floor(Cf));
        Expr hi = cast<int>(floor(hf));

        Expr ld = lf - li;
        Expr Cd = Cf - Ci;
        Expr hd = hf - hi;

        Region lut_bounds = {{Expr(0), lut_dim}, {Expr(0), lut_dim}, {Expr(0), lut_dim}, {Expr(0), 3}};
        Func lut_clamped = BoundaryConditions::repeat_edge(lut, lut_bounds);

        // A clear, textbook implementation of trilinear interpolation.

        // Interpolate along the L axis for the 8 corners of the cube.
        Expr c000 = lut_clamped(li, Ci, hi, c);
        Expr c100 = lut_clamped(li + 1, Ci, hi, c);
        Expr c010 = lut_clamped(li, Ci + 1, hi, c);
        Expr c110 = lut_clamped(li + 1, Ci + 1, hi, c);
        Expr c001 = lut_clamped(li, Ci, hi + 1, c);
        Expr c101 = lut_clamped(li + 1, Ci, hi + 1, c);
        Expr c011 = lut_clamped(li, Ci + 1, hi + 1, c);
        Expr c111 = lut_clamped(li + 1, Ci + 1, hi + 1, c);

        // Interpolate along L.
        Expr c00 = lerp(c000, c100, ld);
        Expr c01 = lerp(c001, c101, ld);
        Expr c10 = lerp(c010, c110, ld);
        Expr c11 = lerp(c011, c111, ld);

        // Interpolate along C.
        Expr c0 = lerp(c00, c10, Cd);
        Expr c1 = lerp(c01, c11, Cd);

        // Interpolate along H.
        output(x, y, c) = lerp(c0, c1, hd);
#else
        // If NO_3D_LUTS is defined, this stage is a simple pass-through.
        output(x, y, c) = lch_input(x, y, c);
#endif
    }
};

#endif // STAGE_COLOR_GRADING_H

