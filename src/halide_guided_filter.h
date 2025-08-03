#ifndef HALIDE_ALGORITHM_GUIDED_FILTER_H
#define HALIDE_ALGORITHM_GUIDED_FILTER_H

#include "Halide.h"
#include <vector>

namespace Halide {

/**
 * Box filter specialization for 2D (single-channel) Funcs.
 */
inline Func box_filter_2d(Func f, int width, int height) {
    Func blur_x("box_blur_x_2d"), blur_y("box_blur_y_2d");
    Var x("bf2d_x"), y("bf2d_y");
    RDom dom_x(-width / 2, width);
    RDom dom_y(-height / 2, height);

    blur_x(x, y) = sum(f(x + dom_x, y));
    blur_y(x, y) = sum(blur_x(x, y + dom_y));
    return blur_y;
}

/**
 * Box filter specialization for 3D (multi-channel) Funcs.
 */
inline Func box_filter_3d(Func f, int width, int height) {
    Func blur_x("box_blur_x_3d"), blur_y("box_blur_y_3d");
    Var x("bf3d_x"), y("bf3d_y"), c("bf3d_c");
    RDom dom_x(-width / 2, width);
    RDom dom_y(-height / 2, height);

    blur_x(x, y, c) = sum(f(x + dom_x, y, c));
    blur_y(x, y, c) = sum(blur_x(x, y + dom_y, c));
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
    GuidedFilterBuilder(Func image, Func guide, Var x, Var y, Var c, int radius = 16, float eps = 0.01f) {
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
        Func mean_I("mean_I"), mean_II("mean_II");
        mean_I(xi, yi) = box_filter_2d(box_I, R, R)(xi, yi);
        mean_II(xi, yi) = box_filter_2d(box_II, R, R)(xi, yi);
        intermediates.push_back(mean_I);
        intermediates.push_back(mean_II);

        Func mean_p("mean_p"), mean_Ip("mean_Ip");
        mean_p(xi, yi, ci) = box_filter_3d(box_p, R, R)(xi, yi, ci);
        mean_Ip(xi, yi, ci) = box_filter_3d(box_Ip, R, R)(xi, yi, ci);
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
