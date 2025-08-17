#ifndef DENOISE_GUIDED_H
#define DENOISE_GUIDED_H

#include "Halide.h"
#include "halide_guided_filter.h" // Reuse box_filter helper
#include "pipeline_helpers.h"
#include <vector>
#include <string>

template<typename T>
class DenoiseGuidedBuilder_T {
private:
    // Helper struct to hold categorized Funcs for a single filter path
    struct GuidedPath {
        Halide::Func result;
        std::vector<Halide::Func> coarse_intermediates;
        std::vector<Halide::Func> fine_intermediates;
    };

    // Helper to build a complete guided filter path for a fixed radius
    GuidedPath create_path(Halide::Func guide, Halide::Var x, Halide::Var y, int radius_val, float eps, int subsample) {
        using namespace Halide;
        Var xi("guided_denoise_xi"), yi("guided_denoise_yi");
        std::string suffix = "_r" + std::to_string(radius_val);

        std::vector<Func> path_coarse, path_fine;

        Func small_guide("small_guide_denoise" + suffix);
        small_guide(xi, yi) = guide(xi * subsample, yi * subsample);
        path_coarse.push_back(small_guide);

        Func box_I("box_I_denoise" + suffix), box_II("box_II_denoise" + suffix);
        box_I(xi, yi) = small_guide(xi, yi);
        box_II(xi, yi) = small_guide(xi, yi) * small_guide(xi, yi);
        path_coarse.push_back(box_I);
        path_coarse.push_back(box_II);

        int R = radius_val / subsample;
        Func mean_I = box_filter_2d(box_I, R, R, "denoise_mean_I" + suffix);
        Func mean_II = box_filter_2d(box_II, R, R, "denoise_mean_II" + suffix);
        path_coarse.push_back(mean_I);
        path_coarse.push_back(mean_II);

        Func var_I("var_I_denoise" + suffix);
        var_I(xi, yi) = mean_II(xi, yi) - mean_I(xi, yi) * mean_I(xi, yi);
        path_coarse.push_back(var_I);

        Func a("a_denoise" + suffix);
        a(xi, yi) = var_I(xi, yi) / (var_I(xi, yi) + eps);
        path_coarse.push_back(a);

        Func b("b_denoise" + suffix);
        b(xi, yi) = mean_I(xi, yi) * (1.0f - a(xi, yi));
        path_coarse.push_back(b);

        Func upsampled_a("upsampled_a_denoise" + suffix), upsampled_b("upsampled_b_denoise" + suffix);
        upsampled_a(x, y) = a(x / subsample, y / subsample);
        upsampled_b(x, y) = b(x / subsample, y / subsample);
        path_fine.push_back(upsampled_a);
        path_fine.push_back(upsampled_b);

        Func result("denoise_guided_result" + suffix);
        result(x, y) = upsampled_a(x, y) * guide(x, y) + upsampled_b(x, y);

        return {result, path_coarse, path_fine};
    }

public:
    Halide::Func output;
    std::vector<Halide::Func> coarse_intermediates;
    std::vector<Halide::Func> fine_intermediates;

    DenoiseGuidedBuilder_T(Halide::Func vst_transformed,
                           Halide::Expr radius_expr,
                           Halide::Var x, Halide::Var y) {

        float eps = 0.0001f;
        int s = 2; // Subsampling factor

        // Create three separate, hard-coded filter paths for different radii
        GuidedPath path1 = create_path(vst_transformed, x, y, 1, eps, s);
        GuidedPath path2 = create_path(vst_transformed, x, y, 2, eps, s);
        GuidedPath path4 = create_path(vst_transformed, x, y, 4, eps, s);

        // Select the result based on the user's input radius
        output = Halide::Func("denoise_guided_result");
        output(x, y) = Halide::select(radius_expr <= 1, path1.result(x, y),
                                      radius_expr <= 2, path2.result(x, y),
                                                        path4.result(x, y));
        fine_intermediates.push_back(path1.result);
        fine_intermediates.push_back(path2.result);
        fine_intermediates.push_back(path4.result);

        // Collect all intermediates from all paths for the main scheduler
        coarse_intermediates.insert(coarse_intermediates.end(), path1.coarse_intermediates.begin(), path1.coarse_intermediates.end());
        coarse_intermediates.insert(coarse_intermediates.end(), path2.coarse_intermediates.begin(), path2.coarse_intermediates.end());
        coarse_intermediates.insert(coarse_intermediates.end(), path4.coarse_intermediates.begin(), path4.coarse_intermediates.end());

        fine_intermediates.insert(fine_intermediates.end(), path1.fine_intermediates.begin(), path1.fine_intermediates.end());
        fine_intermediates.insert(fine_intermediates.end(), path2.fine_intermediates.begin(), path2.fine_intermediates.end());
        fine_intermediates.insert(fine_intermediates.end(), path4.fine_intermediates.begin(), path4.fine_intermediates.end());
    }
};

#endif // DENOISE_GUIDED_H

