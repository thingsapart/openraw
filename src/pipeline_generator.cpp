#include "Halide.h"
#include <stdint.h>

namespace {

using namespace Halide;
using namespace Halide::ConciseCasts;

// Shared variables for concise code
Var x, y, c;

// Helper to average two positive values rounding up
Expr avg(Expr a, Expr b) {
    Type wider = a.type().with_bits(a.type().bits() * 2);
    return cast(a.type(), (cast(wider, a) + b + 1) / 2);
}

// Helper for a 1-2-1 weighted blur. More robust than a box filter.
Expr blur121(Expr a, Expr b, Expr c) {
    return (a + 2.0f * b + c) / 4.0f;
}

// Interleave two functions horizontally or vertically.
Func interleave_x(Func a, Func b) {
    Func out;
    out(x, y) = select((x % 2) == 0, a(x / 2, y), b(x / 2, y));
    return out;
}

Func interleave_y(Func a, Func b) {
    Func out;
    out(x, y) = select((y % 2) == 0, a(x, y / 2), b(x, y / 2));
    return out;
}

class Demosaic : public Halide::Generator<Demosaic> {
public:
    Input<Func> deinterleaved{"deinterleaved", Int(16), 3};
    Output<Func> output{"demosaiced_output", Int(16), 3};

    void generate() {
        Func r_r, g_gr, g_gb, b_b;
        g_gr(x, y) = deinterleaved(x, y, 0); r_r(x, y) = deinterleaved(x, y, 1);
        b_b(x, y) = deinterleaved(x, y, 2); g_gb(x, y) = deinterleaved(x, y, 3);
        Func g_r("g_r"), g_b("g_b");
        Expr gv_r = avg(g_gb(x, y - 1), g_gb(x, y)), gvd_r = absd(g_gb(x, y - 1), g_gb(x, y));
        Expr gh_r = avg(g_gr(x + 1, y), g_gr(x, y)), ghd_r = absd(g_gr(x + 1, y), g_gr(x, y));
        g_r(x, y) = select(ghd_r < gvd_r, gh_r, gv_r);
        Expr gv_b = avg(g_gr(x, y + 1), g_gr(x, y)), gvd_b = absd(g_gr(x, y + 1), g_gr(x, y));
        Expr gh_b = avg(g_gb(x - 1, y), g_gb(x, y)), ghd_b = absd(g_gb(x - 1, y), g_gb(x, y));
        g_b(x, y) = select(ghd_b < gvd_b, gh_b, gv_b);
        Func r_gr("r_gr"), b_gr("b_gr"), r_gb("r_gb"), b_gb("b_gb");
        Expr correction = g_gr(x, y) - avg(g_r(x, y), g_r(x - 1, y)); r_gr(x, y) = correction + avg(r_r(x - 1, y), r_r(x, y));
        correction = g_gr(x, y) - avg(g_b(x, y), g_b(x, y - 1)); b_gr(x, y) = correction + avg(b_b(x, y), b_b(x, y - 1));
        correction = g_gb(x, y) - avg(g_r(x, y), g_r(x, y + 1)); r_gb(x, y) = correction + avg(r_r(x, y), r_r(x, y + 1));
        correction = g_gb(x, y) - avg(g_b(x, y), g_b(x + 1, y)); b_gb(x, y) = correction + avg(b_b(x, y), b_b(x + 1, y));
        Func r_b("r_b"), b_r("b_r");
        correction = g_b(x, y) - avg(g_r(x, y), g_r(x - 1, y + 1)); Expr rp_b = correction + avg(r_r(x, y), r_r(x - 1, y + 1));
        Expr rpd_b = absd(r_r(x, y), r_r(x - 1, y + 1)); correction = g_b(x, y) - avg(g_r(x - 1, y), g_r(x, y + 1));
        Expr rn_b = correction + avg(r_r(x - 1, y), r_r(x, y + 1)); Expr rnd_b = absd(r_r(x - 1, y), r_r(x, y + 1));
        r_b(x, y) = select(rpd_b < rnd_b, rp_b, rn_b); correction = g_r(x, y) - avg(g_b(x, y), g_b(x + 1, y - 1));
        Expr bp_r = correction + avg(b_b(x, y), b_b(x + 1, y - 1)); Expr bpd_r = absd(b_b(x, y), b_b(x + 1, y - 1));
        correction = g_r(x, y) - avg(g_b(x + 1, y), g_b(x, y - 1)); Expr bn_r = correction + avg(b_b(x + 1, y), b_b(x, y - 1));
        Expr bnd_r = absd(b_b(x + 1, y), b_b(x, y - 1)); b_r(x, y) = select(bpd_r < bnd_r, bp_r, bn_r);
        Func r = interleave_y(interleave_x(r_gr, r_r), interleave_x(r_b, r_gb));
        Func g = interleave_y(interleave_x(g_gr, g_r), interleave_x(g_b, g_gb));
        Func b = interleave_y(interleave_x(b_gr, b_r), interleave_x(b_b, b_gb));
        output(x, y, c) = mux(c, {r(x, y), g(x, y), b(x, y)});
    }
};

class CameraPipe : public Halide::Generator<CameraPipe> {
public:
    Input<Buffer<uint16_t, 2>> input{"input"};
    Input<Buffer<float, 2>> ccm{"ccm"};
    Input<float> black_level{"black_level"};
    Input<float> sharpen_amount{"sharpen_amount"};
    Input<float> shadows{"shadows"}, highlights{"highlights"}, midtones{"midtones"};
    Input<Buffer<float>> curve_lut{"curve_lut", 1};
    Output<Buffer<uint8_t, 3>> processed{"processed"};

    Func deinterleaved, luma_guide, curve_adjusted;

    void generate() {
        Func shifted("shifted");
        shifted(x, y) = i16(input(x + 16, y + 12));
        deinterleaved(x, y, c) = mux(c, {shifted(2 * x, 2 * y + 1), shifted(2 * x + 1, 2 * y + 1), shifted(2 * x + 1, 2 * y), shifted(2 * x, 2 * y)});
        auto demosaiced_generator = create<Demosaic>();
        demosaiced_generator->apply(deinterleaved);
        Func demosaiced = demosaiced_generator->output;
        Func corrected_black("corrected_black");
        corrected_black(x, y, c) = demosaiced(x, y, c) - i16(black_level);
        Func corrected_color("corrected_color");
        Expr r_expr = corrected_black(x, y, 0), g_expr = corrected_black(x, y, 1), b_expr = corrected_black(x, y, 2);
        Expr R = ccm(0, 0) * r_expr + ccm(1, 0) * g_expr + ccm(2, 0) * b_expr;
        Expr G = ccm(0, 1) * r_expr + ccm(1, 1) * g_expr + ccm(2, 1) * b_expr;
        Expr B = ccm(0, 2) * r_expr + ccm(1, 2) * g_expr + ccm(2, 2) * b_expr;
        corrected_color(x, y, c) = mux(c, {R, G, B});
        Func tone_input("tone_input");
        tone_input(x, y, c) = f32(corrected_color(x, y, c)) / 65535.0f;
        Func luma("luma");
        luma(x, y) = 0.299f * tone_input(x, y, 0) + 0.587f * tone_input(x, y, 1) + 0.114f * tone_input(x, y, 2);
        Func luma_blur_x("luma_blur_x");
        luma_blur_x(x, y) = (luma(x - 1, y) + luma(x, y) + luma(x + 1, y)) / 3.0f;
        luma_guide(x, y) = (luma_blur_x(x, y - 1) + luma_blur_x(x, y) + luma_blur_x(x, y + 1)) / 3.0f;
        Func local_tone_adjusted("local_tone_adjusted");
        Expr shadow_factor = lerp(1.0f, 1.0f + shadows * 0.5f, 1.0f - luma_guide(x, y));
        Expr highlight_factor = lerp(1.0f, 1.0f - highlights * 0.5f, luma_guide(x, y));
        Expr midtone_factor = 1.0f + luma_guide(x, y) * (1.0f - luma_guide(x, y)) * midtones;
        local_tone_adjusted(x, y, c) = tone_input(x, y, c) * shadow_factor * highlight_factor * midtone_factor;
        Expr val = local_tone_adjusted(x, y, c);
        Expr clamped_val = clamp(val, 0.0f, 1.0f);
        Expr lut_size = curve_lut.dim(0).extent();
        Expr lut_index = cast<int>(clamped_val * (lut_size - 1));
        curve_adjusted(x, y, c) = curve_lut(lut_index);
        Func sharpened("sharpened");
        Func luma_in_for_sharpen("luma_in_for_sharpen");
        luma_in_for_sharpen(x, y) = 0.299f * curve_adjusted(x, y, 0) + 0.587f * curve_adjusted(x, y, 1) + 0.114f * curve_adjusted(x, y, 2);
        Func blur_y("blur_y");
        blur_y(x, y) = blur121(luma_in_for_sharpen(x, y - 1), luma_in_for_sharpen(x, y), luma_in_for_sharpen(x, y + 1));
        Func blurred_luma("blurred_luma");
        blurred_luma(x, y) = blur121(blur_y(x - 1, y), blur_y(x, y), blur_y(x + 1, y));
        Expr detail = luma_in_for_sharpen(x, y) - blurred_luma(x, y);
        Expr sharpened_luma_expr = luma_in_for_sharpen(x, y) + detail * sharpen_amount;
        Expr sharpened_luma_clamped = clamp(sharpened_luma_expr, 0.0f, 1.0f);
        Expr sharpen_gain = select(luma_in_for_sharpen(x, y) > 0.001f, sharpened_luma_clamped / luma_in_for_sharpen(x, y), 1.0f);
        sharpened(x, y, c) = clamp(curve_adjusted(x, y, c) * sharpen_gain, 0.0f, 1.0f);
        processed(x, y, c) = u8_sat(sharpened(x, y, c) * 255.0f);
    }
    void schedule() {
        if (using_autoscheduler()) {
            input.set_estimates({{0, 4000}, {0, 3000}}); ccm.set_estimates({{0, 3}, {0, 3}});
            black_level.set_estimate(64.f); sharpen_amount.set_estimate(1.0f);
            shadows.set_estimate(0.1f); highlights.set_estimate(0.1f); midtones.set_estimate(0.0f);
            curve_lut.set_estimates({{0, 1024}}); processed.set_estimates({{0, 4000-32}, {0, 3000-24}, {0, 3}});
        } else if (get_target().has_gpu_feature()) {
            Var xi, yi;
            processed.compute_root().gpu_tile(x, y, xi, yi, 16, 16).reorder(c, x, y).unroll(c);
            curve_adjusted.compute_at(processed, x).gpu_threads(x, y);
            deinterleaved.compute_at(processed, x).gpu_threads(x, y);
        } else {
            Var yo, yi; int vec = get_target().natural_vector_size<float>();
            processed.compute_root().split(y, yo, yi, 32).parallel(yo).vectorize(x, vec);
            curve_adjusted.compute_at(processed, yi).vectorize(x, vec);
            luma_guide.compute_at(processed, yi).vectorize(x, vec);
            deinterleaved.compute_at(processed, yi).vectorize(x, vec);
        }
    }
};

class RgbPipe : public Halide::Generator<RgbPipe> {
public:
    Input<Buffer<uint8_t, 3>> input{"input"};
    Input<float> sharpen_amount{"sharpen_amount"};
    Input<float> shadows{"shadows"}, highlights{"highlights"}, midtones{"midtones"};
    Input<Buffer<float>> curve_lut{"curve_lut", 1};
    Output<Buffer<uint8_t, 3>> processed{"processed"};

    Func luma_guide, curve_adjusted;

    void generate() {
        // FIX: Add a boundary condition to the input to prevent out-of-bounds reads.
        Func input_bounded = Halide::BoundaryConditions::repeat_edge(input);
        
        Func tone_input("tone_input");
        tone_input(x, y, c) = f32(input_bounded(x, y, c)) / 255.0f;

        Func luma("luma");
        luma(x, y) = 0.299f * tone_input(x, y, 0) + 0.587f * tone_input(x, y, 1) + 0.114f * tone_input(x, y, 2);
        Func luma_blur_x("luma_blur_x");
        luma_blur_x(x, y) = (luma(x - 1, y) + luma(x, y) + luma(x + 1, y)) / 3.0f;
        luma_guide(x, y) = (luma_blur_x(x, y - 1) + luma_blur_x(x, y) + luma_blur_x(x, y + 1)) / 3.0f;
        Func local_tone_adjusted("local_tone_adjusted");
        Expr shadow_factor = lerp(1.0f, 1.0f + shadows * 0.5f, 1.0f - luma_guide(x, y));
        Expr highlight_factor = lerp(1.0f, 1.0f - highlights * 0.5f, luma_guide(x, y));
        Expr midtone_factor = 1.0f + luma_guide(x, y) * (1.0f - luma_guide(x, y)) * midtones;
        local_tone_adjusted(x, y, c) = tone_input(x, y, c) * shadow_factor * highlight_factor * midtone_factor;
        Expr val = local_tone_adjusted(x, y, c);
        Expr clamped_val = clamp(val, 0.0f, 1.0f);
        Expr lut_size = curve_lut.dim(0).extent();
        Expr lut_index = cast<int>(clamped_val * (lut_size - 1));
        curve_adjusted(x, y, c) = curve_lut(lut_index);
        Func sharpened("sharpened");
        Func luma_in_for_sharpen("luma_in_for_sharpen");
        luma_in_for_sharpen(x, y) = 0.299f * curve_adjusted(x, y, 0) + 0.587f * curve_adjusted(x, y, 1) + 0.114f * curve_adjusted(x, y, 2);
        Func blur_y("blur_y");
        blur_y(x, y) = blur121(luma_in_for_sharpen(x, y - 1), luma_in_for_sharpen(x, y), luma_in_for_sharpen(x, y + 1));
        Func blurred_luma("blurred_luma");
        blurred_luma(x, y) = blur121(blur_y(x - 1, y), blur_y(x, y), blur_y(x + 1, y));
        Expr detail = luma_in_for_sharpen(x, y) - blurred_luma(x, y);
        Expr sharpened_luma_expr = luma_in_for_sharpen(x, y) + detail * sharpen_amount;
        Expr sharpened_luma_clamped = clamp(sharpened_luma_expr, 0.0f, 1.0f);
        Expr sharpen_gain = select(luma_in_for_sharpen(x, y) > 0.001f, sharpened_luma_clamped / luma_in_for_sharpen(x, y), 1.0f);
        sharpened(x, y, c) = clamp(curve_adjusted(x, y, c) * sharpen_gain, 0.0f, 1.0f);
        processed(x, y, c) = u8_sat(sharpened(x, y, c) * 255.0f);
    }
    void schedule() {
        if (using_autoscheduler()) {
            input.set_estimates({{0, 4000}, {0, 3000}, {0, 3}});
            sharpen_amount.set_estimate(1.0f);
            shadows.set_estimate(0.1f); highlights.set_estimate(0.1f); midtones.set_estimate(0.0f);
            curve_lut.set_estimates({{0, 1024}});
            processed.set_estimates({{0, 4000}, {0, 3000}, {0, 3}});
        } else if (get_target().has_gpu_feature()) {
            Var xi, yi;
            processed.compute_root().gpu_tile(x, y, xi, yi, 16, 16).reorder(c, x, y).unroll(c);
            curve_adjusted.compute_at(processed, x).gpu_threads(x, y);
        } else {
            Var yo, yi; int vec = get_target().natural_vector_size<float>();
            processed.compute_root().split(y, yo, yi, 32).parallel(yo).vectorize(x, vec);
            curve_adjusted.compute_at(processed, yi).vectorize(x, vec);
            luma_guide.compute_at(processed, yi).vectorize(x, vec);
        }
    }
};
}
HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
HALIDE_REGISTER_GENERATOR(RgbPipe, rgb_pipe)
