#include "Halide.h"
#include "halide_trace_config.h"
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
#include <type_traits>

// Sharpening is disabled to focus on the local adjustments.
#define NO_SHARPEN 1

// Include the individual pipeline stages
#include "stage_denoise.h"
#include "stage_bayer_normalize.h" // NEW
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h" // Now the dispatcher
#include "stage_resize.h"
#include "stage_color_correct.h"
#include "stage_sharpen.h"
#include "stage_local_adjust_laplacian.h" // The new stage
#include "stage_apply_curve.h"
#include "color_tools.h"
#include "stage_color_grading.h"
#include "stage_dehaze.h"
#include "stage_vignette.h"
#include "stage_lens_geometry.h"

#include "pipeline_schedule.h"

// Define guards to disable parts of the lens correction for debugging.
// To use, pass e.g. -D LENS_NO_GEO to the generator's compile command.
// #define LENS_NO_GEO
// #define LENS_NO_DISTORT
// #define LENS_NO_CA

using namespace Halide;

// Shared variables (moved outside anonymous namespace)
Var x("x"), y("y"), c("c"), yi("yi"), yo("yo"), yii("yii"), xi("xi"), xo("xo"), tile("tile");

template <typename T>
class CameraPipeGenerator : public Halide::Generator<CameraPipeGenerator<T>> {
public:
    // --- Generator Parameters for build-time features ---
    GeneratorParam<bool> profile{"profile", false};
    GeneratorParam<bool> trace{"trace", false};

    // --- Define the processing type for this pipeline variant ---
    using proc_type = T;
    const Halide::Type halide_proc_type = Halide::type_of<proc_type>();

    // --- Inputs ---
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<uint16_t, 2>> input{"input"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> cfa_pattern{"cfa_pattern"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> green_balance{"green_balance"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> downscale_factor{"downscale_factor"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> demosaic_algorithm_id{"demosaic_algorithm_id"};
    
    // White Balance and Color Correction Inputs
    typename Generator<CameraPipeGenerator<T>>::template Input<float> wb_r_gain{"wb_r_gain"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> wb_g_gain{"wb_g_gain"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> wb_b_gain{"wb_b_gain"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 2>> color_matrix{"color_matrix"};

    typename Generator<CameraPipeGenerator<T>>::template Input<float> exposure_multiplier{"exposure_multiplier"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ca_correction_strength{"ca_correction_strength"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> denoise_strength{"denoise_strength"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> denoise_eps{"denoise_eps"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> blackLevel{"blackLevel"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> whiteLevel{"whiteLevel"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<uint16_t, 2>> tone_curve_lut{"tone_curve_lut"};

    // Sharpening inputs (kept for API compatibility, but disabled by NO_SHARPEN)
    typename Generator<CameraPipeGenerator<T>>::template Input<float> sharpen_strength{"sharpen_strength"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> sharpen_radius{"sharpen_radius"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> sharpen_threshold{"sharpen_threshold"};

    // New inputs for local adjustments
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ll_detail{"ll_detail"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ll_clarity{"ll_clarity"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ll_shadows{"ll_shadows"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ll_highlights{"ll_highlights"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ll_blacks{"ll_blacks"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ll_whites{"ll_whites"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> ll_debug_level{"ll_debug_level"};

    // New input for global color grading
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 4>> color_grading_lut{"color_grading_lut"};

    // Vignette inputs
    typename Generator<CameraPipeGenerator<T>>::template Input<float> vignette_amount{"vignette_amount"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> vignette_midpoint{"vignette_midpoint"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> vignette_roundness{"vignette_roundness"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> vignette_highlights{"vignette_highlights"};

    // New input for dehaze
    typename Generator<CameraPipeGenerator<T>>::template Input<float> dehaze_strength{"dehaze_strength"};

    // New inputs for Lens Correction & Geometry
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 1>> distortion_lut{"distortion_lut"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ca_red_cyan{"ca_red_cyan"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ca_blue_yellow{"ca_blue_yellow"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> geo_rotate{"geo_rotate"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> geo_scale{"geo_scale"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> geo_aspect{"geo_aspect"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> geo_keystone_v{"geo_keystone_v"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> geo_keystone_h{"geo_keystone_h"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> geo_offset_x{"geo_offset_x"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> geo_offset_y{"geo_offset_y"};


    // --- Output ---
    typename Generator<CameraPipeGenerator<T>>::template Output<Buffer<uint8_t, 3>> processed{"processed"};

    void generate() {
        using namespace Halide::ConciseCasts;
        // ========== THE ALGORITHM ==========
        Expr full_res_width = input.width();
        Expr full_res_height = input.height();

        // The final output dimensions are determined by the downscale factor.
        Expr out_width = cast<int>(full_res_width / downscale_factor);
        Expr out_height = cast<int>(full_res_height / downscale_factor);

        // Create a bounded version of the raw data that is safe to read from.
        Func raw_bounded("raw_bounded");
        raw_bounded = BoundaryConditions::repeat_edge(input, {{0, full_res_width}, {0, full_res_height}});

        // Normalize, convert to float, and apply exposure compensation in one step.
        Func linear_exposed("linear_exposed");
        Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
        linear_exposed(x, y) = (cast<float>(raw_bounded(x, y)) - cast<float>(blackLevel)) * inv_range * exposure_multiplier;

        BayerNormalizeBuilder normalize_builder(linear_exposed, cfa_pattern, green_balance, wb_r_gain, wb_g_gain, wb_b_gain, x, y);
        Func normalized_bayer = normalize_builder.output;

        DenoiseBuilder denoise_builder(linear_exposed, x, y,
                                       denoise_strength, denoise_eps,
                                       this->get_target(), this->using_autoscheduler());
        Func denoised("denoised");
        denoised(x, y) = select(denoise_strength < 0.001f, linear_exposed(x, y), denoise_builder.output(x, y));

        CACorrectBuilder ca_builder(normalized_bayer, x, y,
                                    ca_correction_strength,
                                    full_res_width, full_res_height,
                                    this->get_target(), this->using_autoscheduler());
        Func ca_corrected = ca_builder.output;

        // The input to deinterleave is now guaranteed to be GRBG.
        Func deinterleaved_hi_fi = pipeline_deinterleave(ca_corrected, x, y, c);

        DemosaicDispatcherT<float> demosaic_dispatcher{deinterleaved_hi_fi, demosaic_algorithm_id, x, y, c};
        Func demosaiced = demosaic_dispatcher.output;

        // --- Bicubic Downscaling Step ---
        ResizeBicubicBuilder resize_builder(demosaiced, "resize",
                                            full_res_width, full_res_height,
                                            out_width, out_height, x, y, c);

        Func downscaled("downscaled");
        Expr is_no_op_resize = abs(downscale_factor - 1.0f) < 1e-6f;
        downscaled(x, y, c) = select(is_no_op_resize, demosaiced(x, y, c), resize_builder.output(x, y, c));

        ColorCorrectBuilder_T<T> color_correct_builder(downscaled, halide_proc_type, color_matrix, x, y, c);
        Func corrected_hi_fi = color_correct_builder.output;

        // Create a normalized float version of the input for subsequent stages.
        Func corrected_f("corrected_f");
        if (std::is_same<T, float>::value) {
            corrected_f(x, y, c) = corrected_hi_fi(x, y, c);
        } else {
            corrected_f(x, y, c) = cast<float>(corrected_hi_fi(x, y, c)) / 65535.0f;
        }

        DehazeBuilder dehaze_builder(corrected_f, dehaze_strength, x, y, c);
        Func dehazed = dehaze_builder.output;

        // --- COLOR PROCESSING PIPELINE (Corrected Order) ---
        // 1. Convert from linear sRGB to L*C*h*.
        Func srgb_to_lch = HalideColor::linear_srgb_to_lch(dehazed, x, y, c);

        // 2. Perform local adjustments.
        const int J = 8;
        const int cutover_level = 3;
        LocalLaplacianBuilder local_laplacian_builder(
            srgb_to_lch,
            raw_bounded, color_correct_builder.cc_matrix,
            cfa_pattern, wb_r_gain, wb_g_gain, wb_b_gain, green_balance, exposure_multiplier,
            x, y, c,
            ll_detail, ll_clarity, ll_shadows, ll_highlights, ll_blacks, ll_whites, ll_debug_level,
            blackLevel, whiteLevel,
            out_width, out_height, full_res_width, full_res_height, downscale_factor,
            J, cutover_level);
        Func lch_local_adjusted = local_laplacian_builder.output;

        // 3. Apply the global 3D LUT for color grading.
        ColorGradeBuilder color_grade_builder(lch_local_adjusted, color_grading_lut, color_grading_lut.dim(0).extent(), x, y, c);
        Func lch_final = color_grade_builder.output;

        // 4. Convert the final L''C''H'' back to linear sRGB.
        Func graded_srgb = HalideColor::lch_to_linear_srgb(lch_final, x, y, c);

        // 5. Apply Vignette Correction
        VignetteBuilder vignette_builder(graded_srgb, out_width, out_height,
                                         vignette_amount, vignette_midpoint, vignette_roundness, vignette_highlights,
                                         x, y, c);
        Func vignette_corrected = vignette_builder.output;

        // --- LENS & GEOMETRY CORRECTION STAGE ---
        LensGeometryBuilder lens_geometry_builder(vignette_corrected, x, y, c, out_width, out_height,
                                                  distortion_lut, distortion_lut.dim(0).extent(),
                                                  ca_red_cyan, ca_blue_yellow,
                                                  geo_rotate, geo_scale, geo_aspect,
                                                  geo_keystone_v, geo_keystone_h,
                                                  geo_offset_x, geo_offset_y);
        Func resampled = lens_geometry_builder.output;

        // --- RESAMPLE BYPASS SWITCH ---
        const float e = 1e-6f;
        Expr is_geo_default = abs(geo_rotate) < e && abs(geo_scale - 100.f) < e &&
                              abs(geo_aspect - 1.f) < e && abs(geo_keystone_v) < e &&
                              abs(geo_keystone_h) < e && abs(geo_offset_x) < e &&
                              abs(geo_offset_y) < e;
        // For the distortion LUT, a "default" state is one where all values are 1.0.
        // We can check just the first and last elements as a proxy.
        Expr is_distort_default = abs(distortion_lut(0) - 1.0f) < e &&
                                  abs(distortion_lut(distortion_lut.dim(0).extent() - 1) - 1.0f) < e;
        Expr is_ca_default = abs(ca_red_cyan) < e && abs(ca_blue_yellow) < e;
        Expr is_no_op_resample = is_geo_default && is_distort_default && is_ca_default;

        Func resampled_or_bypass("resampled_or_bypass");
        resampled_or_bypass(x, y, c) = select(is_no_op_resample,
                                              vignette_corrected(x, y, c),
                                              resampled(x, y, c));


        // Convert the float output back to the pipeline's processing type for sharpening.
        Func graded_srgb_proc_type("graded_srgb_proc_type");
        if (std::is_same<T, float>::value) {
            graded_srgb_proc_type(x, y, c) = resampled_or_bypass(x, y, c);
        } else {
            graded_srgb_proc_type(x, y, c) = u16_sat(resampled_or_bypass(x, y, c) * 65535.0f);
        }

        SharpenBuilder_T<T> sharpen_builder(graded_srgb_proc_type, sharpen_strength, sharpen_radius, sharpen_threshold,
                                            out_width, out_height, x, y, c);
        Func sharpened = sharpen_builder.output;

        Func tone_curve_func("tone_curve_func");
        Var lut_x("lut_x_var"), lut_c("lut_c_var");
        tone_curve_func(lut_x, lut_c) = tone_curve_lut(lut_x, lut_c);

        Func curved = pipeline_apply_curve<T>(sharpened, blackLevel, whiteLevel,
                                           tone_curve_func, tone_curve_lut.dim(0).extent(), x, y, c,
                                           this->get_target(), this->using_autoscheduler());

        Func final_stage("final_stage");
        Expr final_val = curved(x, y, c);
        if (std::is_same<proc_type, float>::value) {
            final_stage(x, y, c) = u8_sat(final_val * 255.0f);
        } else {
            final_stage(x, y, c) = u8_sat(final_val >> 8);
        }

        // ========== ESTIMATES ==========
        const int out_width_est = 4000;
        const int out_height_est = 3000;
        input.set_estimates({{0, out_width_est}, {0, out_height_est}});
        cfa_pattern.set_estimate(4); // RGGB is common, but account for new patterns
        green_balance.set_estimate(1.0f);
        downscale_factor.set_estimate(1.0f);
        demosaic_algorithm_id.set_estimate(3);
        ll_debug_level.set_estimate(-1);
        color_matrix.set_estimates({{0, 4}, {0, 3}});
        tone_curve_lut.set_estimates({{0, 65536}, {0, 3}});
        color_grading_lut.set_estimates({{0, 33}, {0, 33}, {0, 33}, {0, 3}});
        distortion_lut.set_estimates({{0, 2048}});
        final_stage.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});

        // ========== SCHEDULE ==========
        // The schedule is now complex enough to warrant its own file.
        schedule_pipeline<T>(this->using_autoscheduler(), this->get_target(),
            denoised, normalized_bayer, ca_builder, deinterleaved_hi_fi, demosaiced, demosaic_dispatcher,
            downscaled, is_no_op_resize, resize_builder,
            corrected_hi_fi, dehazed, resampled, resampled_or_bypass, is_no_op_resample, sharpened, local_laplacian_builder, curved, final_stage,
            color_correct_builder, tone_curve_func, lch_final,
            srgb_to_lch, graded_srgb, vignette_corrected, halide_proc_type,
            x, y, c, xo, xi, yo, yi,
            J, cutover_level);

        processed = final_stage;
    }
};

// Explicitly instantiate the generator for both float and uint16_t.
template class CameraPipeGenerator<float>;
template class CameraPipeGenerator<uint16_t>;

// Create a non-templated alias for the f32 generator to satisfy
// the existing build system which looks for "camera_pipe".
class CameraPipe : public CameraPipeGenerator<float> {};

// Register the old name for the build system, and the new specific names.
HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
HALIDE_REGISTER_GENERATOR(CameraPipeGenerator<float>, camera_pipe_f32)
HALIDE_REGISTER_GENERATOR(CameraPipeGenerator<uint16_t>, camera_pipe_u16)

