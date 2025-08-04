#ifndef STAGE_SHARPEN_H
#define STAGE_SHARPEN_H

#include "Halide.h"
#include "pipeline_helpers.h"
#include <type_traits>
#include <vector>

template <typename T>
class SharpenBuilder_T {
public:
    Halide::Func output;
    std::vector<Halide::Func> intermediates;

    SharpenBuilder_T(Halide::Func input,
                     Halide::Expr sharpen_strength,
                     Halide::Expr sharpen_radius,
                     Halide::Expr sharpen_threshold,
                     Halide::Expr width, Halide::Expr height,
                     Halide::Var x, Halide::Var y, Halide::Var c) {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        output = Func("sharpened");

#ifdef NO_SHARPEN
        output(x, y, c) = input(x, y, c);
#else
        // --- 1. Calculate Luminance ---
        // The input is linear RGB, so a simple weighted sum is correct.
        Func luma("luma");
        Expr r_ch = input(x, y, 0);
        Expr g_ch = input(x, y, 1);
        Expr b_ch = input(x, y, 2);
        luma(x, y) = 0.299f * r_ch + 0.587f * g_ch + 0.114f * b_ch;

        // --- 2. Create 1D Gaussian Kernel ---
        const int kernel_size = 31; // Supports sigma up to ~5.0
        const int kernel_center = kernel_size / 2;
        Func gaussian_kernel("gaussian_kernel");
        Var k("k");
        Expr sigma = max(0.1f, sharpen_radius);
        Expr dist_sq = (k - kernel_center) * (k - kernel_center);
        gaussian_kernel(k) = exp(-dist_sq / (2.0f * sigma * sigma));

        // --- 3. Blur the Luma (Separable Gaussian Blur) ---
        Func luma_clamped = BoundaryConditions::repeat_edge(luma, {{0, width}, {0, height}});
        Func luma_x("luma_x"), blurred_luma("blurred_luma");
        RDom r_blur(0, kernel_size, "sharpen_rdom");

        // Normalize the kernel
        Expr kernel_sum = sum(gaussian_kernel(r_blur.x), "sharpen_kernel_sum");
        
        // Horizontal pass
        luma_x(x, y) = sum(luma_clamped(x + r_blur.x - kernel_center, y) * gaussian_kernel(r_blur.x), "sharpen_blur_x_sum") / kernel_sum;
        
        // Vertical pass
        Func luma_x_clamped = BoundaryConditions::repeat_edge(luma_x, {{0, width}, {0, height}});
        blurred_luma(x, y) = sum(luma_x_clamped(x, y + r_blur.x - kernel_center) * gaussian_kernel(r_blur.x), "sharpen_blur_y_sum") / kernel_sum;

        // --- 4. Unsharp Masking on Luma ---
        Expr detail_luma = luma(x, y) - blurred_luma(x, y);
        Expr edge_mask = smoothstep(0.0f, sharpen_threshold, abs(detail_luma));
        Expr sharpened_luma = luma(x, y) + detail_luma * sharpen_strength * edge_mask;

        // --- 5. Apply Gain to color channels ---
        Expr gain = sharpened_luma / (luma(x, y) + 1e-6f); // Add epsilon for stability
        Expr sharpened_val = input(x, y, c) * gain;
        
        output(x, y, c) = proc_type_sat<T>(sharpened_val);
        
        intermediates.push_back(luma);
        intermediates.push_back(gaussian_kernel);
        intermediates.push_back(luma_x);
        intermediates.push_back(blurred_luma);
#endif
    }
};

#endif // STAGE_SHARPEN_H
