#ifndef STAGE_LENS_GEOMETRY_H
#define STAGE_LENS_GEOMETRY_H

#include "Halide.h"
#include "pipeline_helpers.h"

// Define guards to disable parts of the correction for debugging or performance testing.
// #define LENS_NO_GEO
// #define LENS_NO_CA

class LensGeometryBuilder {
public:
    Halide::Func output;

    LensGeometryBuilder(Halide::Func input_srgb,
                        Halide::Var x, Halide::Var y, Halide::Var c,
                        Halide::Expr out_width, Halide::Expr out_height,
                        Halide::Func distortion_lut, Halide::Expr distortion_max_rd,
                        Halide::Expr ca_red_cyan, Halide::Expr ca_blue_yellow,
                        Halide::Expr geo_rotate, Halide::Expr geo_scale, Halide::Expr geo_aspect,
                        Halide::Expr geo_keystone_v, Halide::Expr geo_keystone_h,
                        Halide::Expr geo_offset_x, Halide::Expr geo_offset_y
                        )
        : output("resampled_srgb")
    {
        using namespace Halide;
        using namespace Halide::ConciseCasts;

        // --- 1. Define the chain of inverse transformations ---
        Expr current_x = cast<float>(x);
        Expr current_y = cast<float>(y);
        Expr center_x = (cast<float>(out_width) - 1.0f) / 2.0f;
        Expr center_y = (cast<float>(out_height) - 1.0f) / 2.0f;

        #ifndef LENS_NO_GEO
        {
            // Translate to center for rotation/scale/keystone
            current_x -= center_x;
            current_y -= center_y;
            Expr angle_rad = geo_rotate * (float)(M_PI / 180.0f);
            Expr cos_a = cos(-angle_rad); Expr sin_a = sin(-angle_rad);
            Expr rot_x = current_x * cos_a - current_y * sin_a;
            Expr rot_y = current_x * sin_a + current_y * cos_a;
            current_x = rot_x; current_y = rot_y;
            Expr kv = geo_keystone_v / 100.f; Expr kh = geo_keystone_h / 100.f;
            Expr denom = 1.f - kv * current_y / center_y - kh * current_x / center_x;
            denom = select(denom > 1e-4f, denom, 1e-4f);
            current_x = current_x / denom; current_y = current_y / denom;
            Expr inv_scale = 100.f / geo_scale;
            current_x *= inv_scale * geo_aspect; current_y *= inv_scale;
            current_x += center_x - geo_offset_x;
            current_y += center_y - geo_offset_y;
        }
        #endif
        
        // --- NEW: Universal Distortion Correction via LUT ---
        {
            Expr dx = current_x - center_x;
            Expr dy = current_y - center_y;
            Expr rd_sq = dx * dx + dy * dy;
            Expr rd = sqrt(rd_sq);

            // Interpolated lookup from the 1D LUT
            Expr lut_size = distortion_lut.dim(0).extent();
            Expr lut_idx_f = (rd / distortion_max_rd) * (cast<float>(lut_size) - 1.0f);
            Expr idx0 = clamp(cast<int>(floor(lut_idx_f)), 0, lut_size - 1);
            Expr idx1 = clamp(idx0 + 1, 0, lut_size - 1);
            Expr weight = lut_idx_f - cast<float>(idx0);
            Expr ru = lerp(distortion_lut(idx0), distortion_lut(idx1), weight);

            Expr scale = ru / (rd + 1e-6f); // Add epsilon to prevent division by zero at center
            
            // This select statement handles the bypass logic. If the LUT represents
            // an identity transform, lut(0) will be 0.
            Expr is_identity = (lut_size > 1 && distortion_lut(0) == 0.0f);
            current_x = select(is_identity, current_x, center_x + dx * scale);
            current_y = select(is_identity, current_y, center_y + dy * scale);
        }
        
        #ifndef LENS_NO_CA
        {
            const float ca_scale = 2e-5f;
            Expr max_radius_sq = max(center_x, center_y) * max(center_x, center_y);
            Expr r2_ca = ((current_x - center_x)*(current_x - center_x) + (current_y - center_y)*(current_y - center_y));
            r2_ca = r2_ca / max_radius_sq;
            Expr r_scale = 1.f + ca_red_cyan * ca_scale * r2_ca;
            Expr b_scale = 1.f + ca_blue_yellow * ca_scale * r2_ca;
            Expr src_x_r = center_x + (current_x - center_x) * r_scale;
            Expr src_y_r = center_y + (current_y - center_y) * r_scale;
            Expr src_x_b = center_x + (current_x - center_x) * b_scale;
            Expr src_y_b = center_y + (current_y - center_y) * b_scale;
            current_x = select(c == 0, src_x_r, c == 2, src_x_b, current_x);
            current_y = select(c == 0, src_y_r, c == 2, src_y_b, current_y);
        }
        #endif
        
        // --- 2. Sample the input image with manual boundary checking and bilinear interpolation ---
        Expr final_src_x = current_x;
        Expr final_src_y = current_y;
        Expr src_min_x = 0.0f, src_max_x = cast<float>(out_width) - 1.0f;
        Expr src_min_y = 0.0f, src_max_y = cast<float>(out_height) - 1.0f;
        Expr in_bounds = (final_src_x >= src_min_x && final_src_x <= src_max_x &&
                          final_src_y >= src_min_y && final_src_y <= src_max_y);
        
        Func safe_input = BoundaryConditions::repeat_edge(input_srgb, {{0, out_width}, {0, out_height}, {0, 3}});
        Expr ix = cast<int>(floor(final_src_x));
        Expr iy = cast<int>(floor(final_src_y));
        Expr fx = final_src_x - ix;
        Expr fy = final_src_y - iy;
        Expr v00 = safe_input(ix, iy, c);
        Expr v10 = safe_input(ix + 1, iy, c);
        Expr v01 = safe_input(ix, iy + 1, c);
        Expr v11 = safe_input(ix + 1, iy + 1, c);
        Expr interp_val = lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fy);
        
        output(x, y, c) = select(in_bounds, interp_val, 0.0f);
    }
};

#endif // STAGE_LENS_GEOMETRY_H
