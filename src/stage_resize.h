#ifndef STAGE_RESIZE_H
#define STAGE_RESIZE_H

#include "Halide.h"
#include <string>
#include <vector>

namespace { // Anonymous namespace for helpers

// Catmull-Rom cubic interpolation kernel
inline Halide::Expr cubic_interp(Halide::Expr p0, Halide::Expr p1, Halide::Expr p2, Halide::Expr p3, Halide::Expr x) {
    return p1 + 0.5f * x * (p2 - p0 +
                              x * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 +
                                   x * (3.0f * (p1 - p2) + p3 - p0)));
}

} // namespace

// This class encapsulates the logic for a bicubic resize operation.
// It exposes its output and intermediate Funcs so the main generator
// can schedule them appropriately within the larger pipeline.
class ResizeBicubicBuilder {
public:
    Halide::Func output;
    Halide::Func interp_y;
    Halide::Var x_coord;

    ResizeBicubicBuilder(Halide::Func input,
                         const std::string& name_prefix,
                         Halide::Expr input_width, Halide::Expr input_height,
                         Halide::Expr output_width, Halide::Expr output_height,
                         Halide::Var x, Halide::Var y, Halide::Var c) 
        : output(name_prefix + "_output"),
          interp_y(name_prefix + "_interp_y"),
          x_coord(name_prefix + "_x_coord")
    {
        using namespace Halide;
    
        // Calculate scale factors.
        Expr scale_x = cast<float>(input_width) / output_width;
        Expr scale_y = cast<float>(input_height) / output_height;
        
        // Get source coordinates for a given output pixel (x, y).
        Expr src_x = (cast<float>(x) + 0.5f) * scale_x - 0.5f;
        Expr src_y = (cast<float>(y) + 0.5f) * scale_y - 0.5f;
        
        Expr ix = cast<int>(floor(src_x));
        Expr iy = cast<int>(floor(src_y));
        Expr fx = src_x - ix;
        Expr fy = src_y - iy;
        
        Func clamped = BoundaryConditions::repeat_edge(input, {{0, input_width}, {0, input_height}});
        
        // --- Pass 1: Interpolate vertically ---
        // This Func computes an interpolated value for any input column `x_coord`
        // at the vertical position `y` from the final output grid.
        interp_y(x_coord, y, c) = cubic_interp(
            clamped(x_coord, iy - 1, c),
            clamped(x_coord, iy,     c),
            clamped(x_coord, iy + 1, c),
            clamped(x_coord, iy + 2, c),
            fy
        );
        
        // --- Pass 2: Interpolate horizontally ---
        // This is the final output Func. It calls the first-pass Func four times
        // to get the vertically-interpolated values for the four neighboring columns.
        output(x, y, c) = cubic_interp(
            interp_y(ix - 1, y, c),
            interp_y(ix,     y, c),
            interp_y(ix + 1, y, c),
            interp_y(ix + 2, y, c),
            fx
        );
    }
};

#endif // STAGE_RESIZE_H
