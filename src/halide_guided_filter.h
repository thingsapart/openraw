#ifndef HALIDE_ALGORITHM_GUIDED_FILTER_H
#define HALIDE_ALGORITHM_GUIDED_FILTER_H

#include "Halide.h"
#include <vector>
#include <string>

namespace Halide {

/**
 * Box filter specialization for 2D (single-channel) Funcs.
 * FIX: Now accepts the Vars `x` and `y` to operate on.
 */
inline Func box_filter_2d(Func f, int width, int height, const std::string &name_prefix, Var x, Var y) {
    Func blur_x(name_prefix + "_blur_x"), blur_y(name_prefix + "_blur_y");
    RDom dom_x(-width / 2, width);
    RDom dom_y(-height / 2, height);

    blur_x(x, y) = sum(f(x + dom_x, y), name_prefix + "_sum_x");
    blur_y(x, y) = sum(blur_x(x, y + dom_y), name_prefix + "_sum_y");
    return blur_y;
}

/**
 * Box filter specialization for 3D (multi-channel) Funcs.
 * FIX: Now accepts the Vars `x`, `y`, and `c` to operate on.
 */
inline Func box_filter_3d(Func f, int width, int height, const std::string &name_prefix, Var x, Var y, Var c) {
    Func blur_x(name_prefix + "_blur_x"), blur_y(name_prefix + "_blur_y");
    RDom dom_x(-width / 2, width);
    RDom dom_y(-height / 2, height);

    blur_x(x, y, c) = sum(f(x + dom_x, y, c), name_prefix + "_sum_x");
    blur_y(x, y, c) = sum(blur_x(x, y + dom_y, c), name_prefix + "_sum_y");
    return blur_y;
}


/** \name GuidedFilterBuilder
 *
 *  A class-based implementation of the edge-preserving Guided Filter.
 *  This structure exposes all intermediate stages (Funcs) to the caller,
 *  allowing for precise, efficient scheduling within a larger pipeline.
 *
 *  See Kaiming He's paper (http://research.microsoft.com/en-us/um/people/kahe/eccv10/)
 *  and the Halide tutorial lesson 11 for details on the algorithm.
 */
class GuidedFilterBuilder {
public:
    // --- Outputs ---
    Func output;
    std::vector<Func> intermediates;

    // --- Constructor ---
    // It now accepts the Vars to use for defining the final output Func.
    GuidedFilterBuilder(Func image, Func guide, Var x, Var y, Var c, int radius = 16, Expr eps = 0.01f) {
        // Internal Vars for intermediate funcs to avoid name conflicts.
        Var xi("gf_xi"), yi("gf_yi"), ci("gf_ci");

        // Subsampling factor for the coarse grid.
        int s = 16;
        int R = radius / s;

        // --- Downsample guide and image ---
        Func small_guide("small_guide"), small_image("small_image");
        small_guide(xi, yi) = guide(xi * s, yi * s);
        small_image(xi, yi, ci) = image(xi * s, yi * s, ci);
        intermediates.push_back(small_guide);
        intermediates.push_back(small_image);

        // --- Compute moments ---
        Func box_I("box_I"), box_II("box_II");
        box_I(xi, yi) = small_guide(xi, yi);
        box_II(xi, yi) = small_guide(xi, yi) * small_guide(xi, yi);
        intermediates.push_back(box_I);
        intermediates.push_back(box_II);

        Func box_p("box_p"), box_Ip("box_Ip");
        box_p(xi, yi, ci) = small_image(xi, yi, ci);
        box_Ip(xi, yi, ci) = small_guide(xi, yi) * small_image(xi, yi, ci);
        intermediates.push_back(box_p);
        intermediates.push_back(box_Ip);

        // --- Convolve moments with a box filter ---
        // FIX: Pass the correct Vars (`xi`, `yi`, `ci`) to the box filter helpers.
        Func mean_I = box_filter_2d(box_I, R, R, "mean_I", xi, yi);
        Func mean_II = box_filter_2d(box_II, R, R, "mean_II", xi, yi);
        intermediates.push_back(mean_I);
        intermediates.push_back(mean_II);

        Func mean_p = box_filter_3d(box_p, R, R, "mean_p", xi, yi, ci);
        Func mean_Ip = box_filter_3d(box_Ip, R, R, "mean_Ip", xi, yi, ci);
        intermediates.push_back(mean_p);
        intermediates.push_back(mean_Ip);

        // --- Compute linear regression coefficients 'a' and 'b' ---
        Func var_I("var_I"), cov_Ip("cov_Ip");
        var_I(xi, yi) = mean_II(xi, yi) - mean_I(xi, yi) * mean_I(xi, yi);
        cov_Ip(xi, yi, ci) = mean_Ip(xi, yi, ci) - mean_I(xi, yi) * mean_p(xi, yi, ci);
        intermediates.push_back(var_I);
        intermediates.push_back(cov_Ip);

        Func a("a"), b("b");
        a(xi, yi, ci) = cov_Ip(xi, yi, ci) / (var_I(xi, yi) + eps);
        b(xi, yi, ci) = mean_p(xi, yi, ci) - a(xi, yi, ci) * mean_I(xi, yi);
        intermediates.push_back(a);
        intermediates.push_back(b);

        // --- Upsample coefficients and apply the filter ---
        Func upsampled_a("upsampled_a"), upsampled_b("upsampled_b");
        // Use the passed-in Vars (x, y, c) for the full-resolution Funcs.
        upsampled_a(x, y, c) = a(x / s, y / s, c);
        upsampled_b(x, y, c) = b(x / s, y / s, c);
        intermediates.push_back(upsampled_a);
        intermediates.push_back(upsampled_b);

        output = Func("guided_filter_result");
        // Define the final output using the passed-in Vars.
        output(x, y, c) = upsampled_a(x, y, c) * guide(x, y) + upsampled_b(x, y, c);
    }
};

}  // namespace Halide

#endif


