#ifndef STAGE_BAYER_NORMALIZE_H
#define STAGE_BAYER_NORMALIZE_H

#include "Halide.h"

class BayerNormalizeBuilder {
public:
    Halide::Func output;

    BayerNormalizeBuilder(Halide::Func input,
                          Halide::Expr cfa_pattern,
                          Halide::Expr green_balance,
                          Halide::Var x, Halide::Var y) {
        using namespace Halide;
        output = Func("bayer_normalized");

        // --- Pure Halide Expr-based LUT ---
        // This is the correct, robust way to implement the offset LUT logic.
        // It avoids using host-side Buffer<> objects inside the pipeline definition,
        // which caused the dimensionality mismatch error.

        // Determine the top-left corner of the 2x2 quad containing the current pixel.
        Expr qx = (x / 2) * 2;
        Expr qy = (y / 2) * 2;

        // For each of the four output pixels in a GRBG quad, we determine the
        // source coordinates based on the input CFA pattern.
        Expr src_x, src_y;

        // Define the logic for each output pixel position within the 2x2 GRBG quad.
        Expr is_y_even = (y % 2 == 0);
        Expr is_x_even = (x % 2 == 0);

        // Location (0,0) in the output quad must be G_r.
        // We find where G_r is in each input pattern.
        Expr dx_gr = select(cfa_pattern == 0, 0,  // GRBG
                     select(cfa_pattern == 1, 1,  // RGGB
                     select(cfa_pattern == 2, 1,  // GBRG
                     select(cfa_pattern == 3, 0,  // BGGR
                                              1))));// RGBG
        Expr dy_gr = select(cfa_pattern == 0, 0,
                     select(cfa_pattern == 1, 0,
                     select(cfa_pattern == 2, 1,
                     select(cfa_pattern == 3, 1,
                                              0))));

        // Location (0,1) in the output quad must be R.
        Expr dx_r = select(cfa_pattern == 0, 1,
                    select(cfa_pattern == 1, 0,
                    select(cfa_pattern == 2, 0,
                    select(cfa_pattern == 3, 1,
                                             0))));
        Expr dy_r = select(cfa_pattern == 0, 0,
                    select(cfa_pattern == 1, 0,
                    select(cfa_pattern == 2, 1,
                    select(cfa_pattern == 3, 1,
                                             0))));

        // Location (1,0) in the output quad must be B.
        Expr dx_b = select(cfa_pattern == 0, 0,
                    select(cfa_pattern == 1, 1,
                    select(cfa_pattern == 2, 1,
                    select(cfa_pattern == 3, 0,
                                             0))));
        Expr dy_b = select(cfa_pattern == 0, 1,
                    select(cfa_pattern == 1, 1,
                    select(cfa_pattern == 2, 0,
                    select(cfa_pattern == 3, 0,
                                             1))));

        // Location (1,1) in the output quad must be G_b.
        Expr dx_gb = select(cfa_pattern == 0, 1,
                     select(cfa_pattern == 1, 0,
                     select(cfa_pattern == 2, 0,
                     select(cfa_pattern == 3, 1,
                                              1))));
        Expr dy_gb = select(cfa_pattern == 0, 1,
                     select(cfa_pattern == 1, 1,
                     select(cfa_pattern == 2, 0,
                     select(cfa_pattern == 3, 0,
                                              1))));

        // Select the final source coordinates based on the output pixel's position.
        src_x = select(is_y_even && is_x_even,   qx + dx_gr,
                select(is_y_even && !is_x_even,  qx + dx_r,
                select(!is_y_even && is_x_even,  qx + dx_b,
                                                 qx + dx_gb)));

        src_y = select(is_y_even && is_x_even,   qy + dy_gr,
                select(is_y_even && !is_x_even,  qy + dy_r,
                select(!is_y_even && is_x_even,  qy + dy_b,
                                                 qy + dy_gb)));

        // --- Read the pixel and apply green balance ---
        Expr val = input(src_x, src_y);

        // Apply green balance only to the G_b pixels.
        // In the target GRBG pattern, G_b is at (odd, odd) locations.
        Expr is_gb_location = !is_y_even && !is_x_even;

        output(x, y) = select(is_gb_location, val * green_balance, val);
    }
};

#endif // STAGE_BAYER_NORMALIZE_H
