#include "Halide.h"
#include "halide_trace_config.h"
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
#include <type_traits>

// Keep sharpening disabled as requested for this feature update.
#define NO_SHARPEN 1

// Include the individual pipeline stages
#include "stage_denoise.h"
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h" // Now the dispatcher
#include "stage_color_correct.h"
#include "stage_sharpen.h"
#include "stage_local_adjust_laplacian.h" // The new stage
#include "stage_apply_curve.h"
// DO NOT INCLUDE THE SCHEDULE HERE - It must be included inside the generate() method.

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
    typename Generator<CameraPipeGenerator<T>>::template Input<int> demosaic_algorithm_id{"demosaic_algorithm_id"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 2>> matrix_3200{"matrix_3200"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 2>> matrix_7000{"matrix_7000"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> color_temp{"color_temp"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> tint{"tint"};
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
        Expr out_width = input.width() - 32;
        Expr out_height = input.height() - 24;

        Func initial_raw("initial_raw");
        initial_raw(x, y) = input(x + 16, y + 12);

        // --- Boundary Condition on the Ultimate Source ---
        // This is the key fix. By applying the boundary condition here, all
        // downstream stages (denoise, CA correct) can safely read beyond the
        // original bounds, allowing their large stencils to work correctly
        // within a strip-based schedule.
        Func raw_bounded("raw_bounded");
        raw_bounded = BoundaryConditions::repeat_edge(initial_raw, {{0, out_width}, {0, out_height}});
        
        // --- High-Fidelity Path ---
        // The denoise builder now consumes the safely bounded raw image.
        DenoiseBuilder_T<T> denoise_builder{raw_bounded, x, y,
                                            denoise_strength, denoise_eps,
                                            blackLevel, whiteLevel,
                                            this->get_target(), this->using_autoscheduler()};
        
        Func denoised_raw("denoised_raw");
        Expr passthrough_val_hi_fi;
        if (std::is_same<T, float>::value) {
            Expr inv_range = 1.0f / (cast<float>(whiteLevel) - cast<float>(blackLevel));
            passthrough_val_hi_fi = (cast<float>(raw_bounded(x, y)) - blackLevel) * inv_range;
        } else {
            passthrough_val_hi_fi = raw_bounded(x, y);
        }
        denoised_raw(x, y) = select(denoise_strength < 0.001f, passthrough_val_hi_fi, denoise_builder.output(x, y));
        
        CACorrectBuilder_T<T> ca_builder{denoised_raw, x, y,
                                       ca_correction_strength, blackLevel, whiteLevel,
                                       out_width, out_height,
                                       this->get_target(), this->using_autoscheduler()};
        Func ca_corrected = ca_builder.output;

        Func deinterleaved_hi_fi = pipeline_deinterleave(ca_corrected, x, y, c);
        
        DemosaicDispatcherT<T> demosaic_dispatcher{deinterleaved_hi_fi, demosaic_algorithm_id, x, y, c};
        Func demosaiced = demosaic_dispatcher.output;
        
        ColorCorrectBuilder_T<T> color_correct_builder(demosaiced, halide_proc_type, matrix_3200, matrix_7000,
                                                       color_temp, tint, x, y, c, whiteLevel, blackLevel);
        Func corrected_hi_fi = color_correct_builder.output;

        SharpenBuilder_T<T> sharpen_builder(corrected_hi_fi, sharpen_strength, sharpen_radius, sharpen_threshold,
                                            out_width, out_height, x, y, c);
        Func sharpened = sharpen_builder.output;
        
        // --- Forked Local Laplacian Stage ---
        // The low-fi path also uses the bounded raw image.
        LocalLaplacianBuilder_T<T> local_laplacian_builder(
            sharpened, raw_bounded, color_correct_builder.cc_matrix,
            x, y, c,
            ll_detail, ll_clarity, ll_shadows, ll_highlights, ll_blacks, ll_whites,
            blackLevel, whiteLevel,
            out_width, out_height);
        Func local_adjustments = local_laplacian_builder.output;

        // --- Final Tone Curve and Output ---
        Func tone_curve_func("tone_curve_func");
        Var lut_x("lut_x"), lut_c("lut_c");
        tone_curve_func(lut_x, lut_c) = tone_curve_lut(lut_x, lut_c);

        Func curved = pipeline_apply_curve<T>(local_adjustments, blackLevel, whiteLevel,
                                           tone_curve_func, tone_curve_lut.dim(0).extent(), x, y, c,
                                           this->get_target(), this->using_autoscheduler());
        
        Func final_stage("final_stage");
        final_stage(x, y, c) = curved(x, y, c);

        // ========== ESTIMATES ==========
        const int out_width_est = 2592 - 32;
        const int out_height_est = 1968 - 24;
        input.set_estimates({{0, 2592}, {0, 1968}});
        demosaic_algorithm_id.set_estimate(3);
        matrix_3200.set_estimates({{0, 4}, {0, 3}});
        matrix_7000.set_estimates({{0, 4}, {0, 3}});
        tone_curve_lut.set_estimates({{0, 65536}, {0, 3}});
        final_stage.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});
        // Other params are fine with defaults.

        // ========== SCHEDULE ==========
        #include "pipeline_schedule.h"
        
        processed = final_stage;
    }
};

// Explicitly instantiate the generator for both float and uint16_t.
class CameraPipe_u16 : public CameraPipeGenerator<uint16_t> {};
HALIDE_REGISTER_GENERATOR(CameraPipe_u16, camera_pipe_u16)

class CameraPipe_f32 : public CameraPipeGenerator<float> {};
HALIDE_REGISTER_GENERATOR(CameraPipe_f32, camera_pipe_f32)

class CameraPipe : public CameraPipeGenerator<uint16_t> {};
HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
