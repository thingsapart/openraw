#ifndef DENOISE_NLMEANS_H
#define DENOISE_NLMEANS_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include <vector>

template<typename T>
class DenoiseNLMeansBuilder_T {
public:
    Halide::Func output;
    std::vector<Halide::Func> coarse_intermediates; // Always empty for NL-Means
    std::vector<Halide::Func> fine_intermediates;
    Halide::RDom r_search, r_patch;

    DenoiseNLMeansBuilder_T(Halide::Func vst_transformed,
                            Halide::Expr search_area,
                            Halide::Expr patch_size,
                            Halide::Expr strength,
                            Halide::Expr width,
                            Halide::Expr height,
                            Halide::Var x, Halide::Var y) {
        using namespace Halide;

        // Create an explicit region of definition for the boundary condition.
        Region bounds = {{0, width}, {0, height}};
        Func clamped = BoundaryConditions::repeat_edge(vst_transformed, bounds);
        // FIX: 'clamped' is a local helper. Do not add it to the intermediates list.
        // This prevents the generator from trying to schedule it, allowing it to be
        // inlined by default, which is the correct behavior for this type of Func.

        // Algorithmic parameters
        // h is the filtering parameter, linked to user strength.
        // A base_h of 0.6 is a reasonable starting point for VST data with sigma ~1.
        Expr h = 0.6f * strength * strength;
        Expr h2 = h*h;
        // Avoid division by zero if strength is zero
        Expr inv_h2 = select(h2 > 0.f, 1.0f / h2, 0.f);

        // Create the reduction domains. We store them as members to access their RVars during scheduling.
        Expr patch_radius = patch_size / 2;
        Expr search_radius = search_area / 2;
        r_patch = RDom(-patch_radius, patch_size, -patch_radius, patch_size, "nl_patch");
        r_search = RDom(-search_radius, search_area, -search_radius, search_area, "nl_search");

        // The ssd_func computes the sum of squared differences for a single pair of patches.
        Func ssd_func("ssd_func");
        Var sx("nl_sx"), sy("nl_sy"); // Explicit vars for search domain
        Expr p1 = clamped(x + r_patch.x, y + r_patch.y);
        Expr p2 = clamped(x + sx + r_patch.x, y + sy + r_patch.y);
        ssd_func(x, y, sx, sy) = sum(pow(p1 - p2, 2.0f), "ssd_sum");
        fine_intermediates.push_back(ssd_func);

        // Compute the weight `w` for each patch in the search window using the ssd_func.
        Expr ssd = ssd_func(x, y, r_search.x, r_search.y);
        Expr patch_area = cast<float>(patch_size * patch_size);
        Expr w = exp(-ssd * inv_h2 / patch_area);

        // Sum of weights and sum of weighted pixel values over the search window.
        Func total_weight("nlmeans_total_weight");
        total_weight(x, y) = sum(w, "nl_total_weight_sum");
        fine_intermediates.push_back(total_weight);

        Func weighted_sum("nlmeans_weighted_sum");
        weighted_sum(x, y) = sum(w * clamped(x + r_search.x, y + r_search.y), "nl_weighted_sum");
        fine_intermediates.push_back(weighted_sum);

        // The final NL-Means result is the normalized sum
        output = Func("denoise_nlmeans_result");
        output(x, y) = weighted_sum(x, y) / total_weight(x, y);
    }
};

#endif // DENOISE_NLMEANS_H

