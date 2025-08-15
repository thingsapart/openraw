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
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h" // Now the dispatcher
#include "stage_resize.h"
#include "stage_color_correct.h"
#include "stage_sharpen.h"
#include "stage_local_adjust_laplacian.h" // The new stage
#include "stage_apply_curve.h"

#include "pipeline_schedule.h"

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
    typename Generator<CameraPipeGenerator<T>>::template Input<float> downscale_factor{"downscale_factor"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> demosaic_algorithm_id{"demosaic_algorithm_id"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 2>> matrix_3200{"matrix_3200"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 2>> matrix_7000{"matrix_7000"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> color_temp{"color_temp"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> tint{"tint"};
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

    // --- Output ---
    typename Generator<CameraPipeGenerator<T>>::template Output<Buffer<uint8_t, 3>> processed{"processed"};

    void generate() {
        using namespace Halide::ConciseCasts;
        // ========== THE ALGORITHM ==========
        Expr full_res_width = input.width() - 32;
        Expr full_res_height = input.height() - 24;

        // The final output dimensions are determined by the downscale factor.
        Expr out_width = cast<int>(full_res_width / downscale_factor);
        Expr out_height = cast<int>(full_res_height / downscale_factor);

        // Define the crop to the valid sensor area.
        Func initial_raw("initial_raw");
        initial_raw(x, y) = input(x + 16, y + 12);

        // Create a bounded version of the raw data that is safe to read from.
        Func raw_bounded("raw_bounded");
        raw_bounded = BoundaryConditions::repeat_edge(initial_raw, {{0, full_res_width}, {0, full_res_height}});

        // Normalize, convert to float, and apply exposure compensation in one step.
        Func linear_exposed("linear_exposed");
        Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
        linear_exposed(x, y) = (cast<float>(raw_bounded(x, y)) - cast<float>(blackLevel)) * inv_range * exposure_multiplier;

        DenoiseBuilder denoise_builder(linear_exposed, x, y,
                                       denoise_strength, denoise_eps,
                                       this->get_target(), this->using_autoscheduler());
        Func denoised("denoised");
        denoised(x, y) = select(denoise_strength < 0.001f, linear_exposed(x, y), denoise_builder.output(x, y));

        CACorrectBuilder ca_builder(denoised, x, y,
                                    ca_correction_strength,
                                    full_res_width, full_res_height,
                                    this->get_target(), this->using_autoscheduler());
        Func ca_corrected = ca_builder.output;

        Func deinterleaved_hi_fi = pipeline_deinterleave(ca_corrected, x, y, c);

        DemosaicDispatcherT<float> demosaic_dispatcher{deinterleaved_hi_fi, demosaic_algorithm_id, x, y, c};
        Func demosaiced = demosaic_dispatcher.output;

        // --- Bicubic Downscaling Step ---
        ResizeBicubicBuilder resize_builder(demosaiced, "resize",
                                            full_res_width, full_res_height,
                                            out_width, out_height, x, y, c);
        
        Func downscaled("downscaled");
        Expr is_no_op = abs(downscale_factor - 1.0f) < 1e-6f;
        downscaled(x, y, c) = select(is_no_op, demosaiced(x, y, c), resize_builder.output(x, y, c));

        ColorCorrectBuilder_T<T> color_correct_builder(downscaled, halide_proc_type, matrix_3200, matrix_7000,
                                                       color_temp, tint, x, y, c, whiteLevel, blackLevel);
        Func corrected_hi_fi = color_correct_builder.output;

        SharpenBuilder_T<T> sharpen_builder(corrected_hi_fi, sharpen_strength, sharpen_radius, sharpen_threshold,
                                            out_width, out_height, x, y, c);
        Func sharpened = sharpen_builder.output;

        // --- Local Laplacian Stage ---
        // Create a normalized float version of the input for the builder.
        Func laplacian_input("laplacian_input");
        if (std::is_same<T, float>::value) {
            laplacian_input(x, y, c) = sharpened(x, y, c);
        } else {
            laplacian_input(x, y, c) = cast<float>(sharpened(x, y, c)) / 65535.0f;
        }

        // The builder is now untemplated and works only on normalized floats.
        const int J = 8;
        const int cutover_level = 3; // Set J < cutover_level to test the pure hi-fi path
        LocalLaplacianBuilder local_laplacian_builder(
            laplacian_input, raw_bounded, color_correct_builder.cc_matrix,
            x, y, c,
            ll_detail, ll_clarity, ll_shadows, ll_highlights, ll_blacks, ll_whites,
            blackLevel, whiteLevel,
            out_width, out_height, full_res_width, full_res_height, downscale_factor,
            J, cutover_level);
        Func local_adjustments_f = local_laplacian_builder.output;

        // Convert the float output back to the pipeline's processing type.
        Func local_adjustments("local_adjustments");
        if (std::is_same<T, float>::value) {
            local_adjustments(x, y, c) = local_adjustments_f(x, y, c);
        } else {
            local_adjustments(x, y, c) = u16_sat(local_adjustments_f(x, y, c) * 65535.0f);
        }

        Func tone_curve_func("tone_curve_func");
        Var lut_x("lut_x_var"), lut_c("lut_c_var");
        tone_curve_func(lut_x, lut_c) = tone_curve_lut(lut_x, lut_c);

        Func curved = pipeline_apply_curve<T>(local_adjustments, blackLevel, whiteLevel,
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
        const int out_width_est = 2592 - 32;
        const int out_height_est = 1968 - 24;
        input.set_estimates({{0, 2592}, {0, 1968}});
        downscale_factor.set_estimate(1.0f);
        demosaic_algorithm_id.set_estimate(3);
        matrix_3200.set_estimates({{0, 4}, {0, 3}});
        matrix_7000.set_estimates({{0, 4}, {0, 3}});
        tone_curve_lut.set_estimates({{0, 65536}, {0, 3}});
        // The output estimate must not depend on an Input parameter for the autoscheduler,
        // so we use the full-resolution estimates. Halide's bounds inference will
        // handle the actual size during JIT compilation.
        final_stage.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});

        // ========== SCHEDULE ==========
        // The schedule is now complex enough to warrant its own file.
        schedule_pipeline<T>(this->using_autoscheduler(), this->get_target(),
            denoised, ca_builder, deinterleaved_hi_fi, demosaiced, demosaic_dispatcher,
            downscaled, is_no_op, resize_builder,
            corrected_hi_fi, sharpened, local_laplacian_builder, curved, final_stage,
            color_correct_builder, tone_curve_func, halide_proc_type,
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
