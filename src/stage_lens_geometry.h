#ifndef STAGE_LENS_GEOMETRY_H
#define STAGE_LENS_GEOMETRY_H

#include "Halide.h"
#include "pipeline_helpers.h"

// Define guards to disable parts of the correction for debugging or performance testing.
// #define LENS_NO_DISTORT
// #define LENS_NO_GEO
// #define LENS_NO_CA

class LensGeometryBuilder {
public:
    Halide::Func output;

    LensGeometryBuilder(Halide::Func input_srgb,
                        Halide::Var x, Halide::Var y, Halide::Var c,
                        Halide::Expr out_width, Halide::Expr out_height,
                        Halide::Expr dist_k1, Halide::Expr dist_k2, Halide::Expr dist_k3,
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
        // We start with the output coordinates of the final resampled image
        // and work backwards to find the corresponding source pixel in the input.

        Expr current_x = cast<float>(x);
        Expr current_y = cast<float>(y);

        // Center coordinates for transformations that are origin-dependent.
        Expr center_x = (cast<float>(out_width) - 1.0f) / 2.0f;
        Expr center_y = (cast<float>(out_height) - 1.0f) / 2.0f;

        #ifndef LENS_NO_GEO
        {
            // Translate to center for rotation/scale/keystone
            current_x -= center_x;
            current_y -= center_y;

            // 1. Inverse Rotation
            Expr angle_rad = geo_rotate * (float)(M_PI / 180.0f);
            Expr cos_a = cos(-angle_rad);
            Expr sin_a = sin(-angle_rad);
            Expr rot_x = current_x * cos_a - current_y * sin_a;
            Expr rot_y = current_x * sin_a + current_y * cos_a;
            current_x = rot_x;
            current_y = rot_y;

            // 2. Inverse Keystone (Perspective)
            Expr kv = geo_keystone_v / 100.f;
            Expr kh = geo_keystone_h / 100.f;
            Expr denom = 1.f - kv * current_y / center_y - kh * current_x / center_x;
            denom = select(denom > 1e-4f, denom, 1e-4f);
            current_x = current_x / denom;
            current_y = current_y / denom;

            // 3. Inverse Scale & Aspect
            Expr inv_scale = 100.f / geo_scale;
            current_x *= inv_scale * geo_aspect;
            current_y *= inv_scale;

            // Translate back from center and apply inverse offset
            current_x += center_x - geo_offset_x;
            current_y += center_y - geo_offset_y;
        }
        #endif
        
        #ifndef LENS_NO_DISTORT
        {
            // Normalize coordinates to be relative to the center for radial model
            Expr norm_distort_x = (current_x - center_x);
            Expr norm_distort_y = (current_y - center_y);

            // To make distortion independent of image size, normalize radius by the smaller dimension
            Expr max_radius = max(out_width, out_height) / 2.0f;
            Expr r_sq_norm = (norm_distort_x*norm_distort_x + norm_distort_y*norm_distort_y) / (max_radius * max_radius);

            Expr radial_dist = 1.0f + dist_k1*r_sq_norm + dist_k2*r_sq_norm*r_sq_norm + dist_k3*r_sq_norm*r_sq_norm*r_sq_norm;

            current_x = center_x + norm_distort_x * radial_dist;
            current_y = center_y + norm_distort_y * radial_dist;
        }
        #endif
        
        #ifndef LENS_NO_CA
        {
            const float ca_scale = 2e-5f;
            Expr max_radius_sq = max(center_x, center_y) * max(center_x, center_y);
            Expr r2_ca = ((current_x - center_x)*(current_x - center_x) + (current_y - center_y)*(current_y - center_y));
            r2_ca = r2_ca / max_radius_sq; // Normalize for consistent feel
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

        Expr v00 = safe_input(ix,     iy,     c);
        Expr v10 = safe_input(ix + 1, iy,     c);
        Expr v01 = safe_input(ix,     iy + 1, c);
        Expr v11 = safe_input(ix + 1, iy + 1, c);
        Expr interp_val = lerp(lerp(v00, v10, fx),
                               lerp(v01, v11, fx),
                               fy);
        
        output(x, y, c) = select(in_bounds, interp_val, 0.0f);
    }
};

#endif // STAGE_LENS_GEOMETRY_H
