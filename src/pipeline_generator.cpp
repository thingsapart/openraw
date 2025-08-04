#include "Halide.h"
#include "halide_trace_config.h"
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
#include <type_traits>

// Include the individual pipeline stages
#include "stage_denoise.h"
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h" // Now the dispatcher
#include "stage_color_correct.h"
#include "stage_sharpen.h"
#include "stage_apply_curve.h"

using namespace Halide;

// Shared variables (moved outside anonymous namespace)
Var x("x"), y("y"), c("c"), yi("yi"), yo("yo"), yii("yii"), xi("xi");

template <typename T>
class CameraPipeGenerator : public Halide::Generator<CameraPipeGenerator<T>> {
public:
    // --- Generator Parameters for build-time features ---
    // These are switches that control what features are compiled into the pipeline.
    // The Halide build system automatically enables features when it sees these
    // specific names (e.g., 'profile', 'trace') set to true.
    GeneratorParam<bool> profile{"profile", false};
    GeneratorParam<bool> trace{"trace", false};

    // --- Define the processing type for this pipeline variant ---
    using proc_type = T;
    const Halide::Type halide_proc_type = Halide::type_of<proc_type>();

    // --- Inputs ---
    // The `typename` keyword disambiguates that 'Input' is a type.
    // The `template` keyword disambiguates that 'Input' is a template member.
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<uint16_t, 2>> input{"input"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> demosaic_algorithm_id{"demosaic_algorithm_id"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 2>> matrix_3200{"matrix_3200"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<float, 2>> matrix_7000{"matrix_7000"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> color_temp{"color_temp"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> tint{"tint"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> sharpen_strength{"sharpen_strength"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> sharpen_radius{"sharpen_radius"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> sharpen_threshold{"sharpen_threshold"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ca_correction_strength{"ca_correction_strength"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> denoise_strength{"denoise_strength"};
    typename Generator<CameraPipeGenerator<T>>::template Input<float> denoise_eps{"denoise_eps"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> blackLevel{"blackLevel"};
    typename Generator<CameraPipeGenerator<T>>::template Input<int> whiteLevel{"whiteLevel"};
    typename Generator<CameraPipeGenerator<T>>::template Input<Buffer<uint16_t, 2>> tone_curve_lut{"tone_curve_lut"};

    // --- Output ---
    typename Generator<CameraPipeGenerator<T>>::template Output<Buffer<uint8_t, 3>> processed{"processed"};

    void generate() {
        using namespace Halide::ConciseCasts;
        // ========== THE ALGORITHM ==========
        Expr out_width_est = 2592 - 32;
        Expr out_height_est = 1968 - 24;
        
        // Stage -1: Denoise raw data as a completely separate, materialized step.
        Func initial_raw("initial_raw");
        initial_raw(x, y) = input(x + 16, y + 12);
        
        DenoiseBuilder_T<T> denoise_builder{initial_raw, x, y,
                                            denoise_strength, denoise_eps,
                                            blackLevel, whiteLevel,
                                            this->get_target(), this->using_autoscheduler()};
        
        Func denoised_raw("denoised_raw");
        Expr passthrough_val;
        if (std::is_same<T, float>::value) {
            passthrough_val = (cast<float>(initial_raw(x, y)) - blackLevel) / (whiteLevel - blackLevel);
        } else {
            passthrough_val = initial_raw(x, y);
        }
        
        denoised_raw(x, y) = select(denoise_strength < 0.001f,
                                    passthrough_val,
                                    denoise_builder.output(x, y));
        
        // Stage 1.5: Chromatic Aberration Correction
        // This now consumes the output of the denoise stage directly.
        CACorrectBuilder_T<T> ca_builder{denoised_raw, x, y,
                                       ca_correction_strength,
                                       blackLevel, whiteLevel,
                                       out_width_est, out_height_est,
                                       this->get_target(), this->using_autoscheduler()};
        Func ca_corrected = ca_builder.output;

        // Stage 2: Deinterleave the Bayer pattern
        Func deinterleaved = pipeline_deinterleave(ca_corrected, x, y, c);

        // Stage 3: Demosaic using the templated dispatcher
        DemosaicDispatcherT<T> demosaic_dispatcher{deinterleaved, demosaic_algorithm_id, x, y, c};
        Func demosaiced = demosaic_dispatcher.output;

        // Stage 4: Color correction
        ColorCorrectBuilder_T<T> color_correct_builder(demosaiced, halide_proc_type, matrix_3200, matrix_7000,
                                                       color_temp, tint, x, y, c, whiteLevel, blackLevel);
        Func corrected = color_correct_builder.output;

        // Stage 5: Sharpen the image (now on linear data)
        SharpenBuilder_T<T> sharpen_builder(corrected, sharpen_strength, sharpen_radius, sharpen_threshold,
                                            out_width_est, out_height_est, x, y, c);
        Func sharpened = sharpen_builder.output;

        // Stage 6: Apply tone curve from LUT (now after sharpening)
        Func tone_curve_func("tone_curve_func");
        Var lut_x("lut_x_var"), lut_c("lut_c_var");
        tone_curve_func(lut_x, lut_c) = tone_curve_lut(lut_x, lut_c);
        Expr lut_size_expr = tone_curve_lut.dim(0).extent();

        Func curved = pipeline_apply_curve<T>(sharpened, blackLevel, whiteLevel,
                                           tone_curve_func, lut_size_expr, x, y, c,
                                           this->get_target(), this->using_autoscheduler());
        
        // Final stage: Create an explicit Func for the output, consuming the curved result.
        Func final_stage("final_stage");
        Expr final_val = curved(x, y, c);
        if (std::is_same<proc_type, float>::value) {
            final_stage(x, y, c) = u8_sat(final_val * 255.0f);
        } else {
            final_stage(x, y, c) = u8_sat(final_val >> 8);
        }

        // ========== Pipeline-level Tracing and Profiling Control ==========
        if (trace) {
            final_stage.trace_stores();
        }

        // ========== ESTIMATES ==========
        input.set_estimates({{0, 2592}, {0, 1968}});
        demosaic_algorithm_id.set_estimate(3);
        matrix_3200.set_estimates({{0, 4}, {0, 3}});
        matrix_7000.set_estimates({{0, 4}, {0, 3}});
        tone_curve_lut.set_estimates({{0, 65536}, {0, 3}});
        color_temp.set_estimate(3700);
        tint.set_estimate(0.0f);
        sharpen_strength.set_estimate(1.0f);
        sharpen_radius.set_estimate(1.0f);
        sharpen_threshold.set_estimate(0.02f);
        ca_correction_strength.set_estimate(1.0f);
        denoise_strength.set_estimate(0.5f);
        denoise_eps.set_estimate(0.01f);
        blackLevel.set_estimate(25);
        whiteLevel.set_estimate(1023);
        final_stage.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});

        // ========== SCHEDULE ==========
        if (this->using_autoscheduler()) {
            // Let the auto-scheduler do its thing.
        } else if (this->get_target().has_gpu_feature()) {
            // Manual GPU schedule
            Var xi, yi;
            int tile_x = 28, tile_y = 12;

            denoised_raw.compute_root().gpu_tile(x, y, xi, yi, 16, 8);
            ca_corrected.compute_root().gpu_tile(x, y, xi, yi, 16, 8);
            sharpened.compute_root().reorder(c, x, y).gpu_tile(x, y, xi, yi, tile_x, tile_y);

            final_stage.compute_root().reorder(c, x, y).gpu_tile(x, y, xi, yi, tile_x, tile_y);
            curved.compute_at(final_stage, x).gpu_threads(x, y);

            // Helpers for root stages must also be scheduled for GPU
            corrected.compute_at(sharpened, x).gpu_threads(x,y);
            demosaiced.compute_at(sharpened, x).gpu_threads(x,y);
            deinterleaved.compute_at(sharpened, x).gpu_threads(x,y);

        } else {
            // Manual CPU schedule: High-performance strip-fused schedule with sliding windows.
            int vec = this->get_target().natural_vector_size(halide_proc_type);
            Expr strip_size = 32;

            // --- Root-level pre-computation for complex stages ---
            denoised_raw.compute_root().parallel(y).vectorize(x, vec);
            ca_corrected.compute_root().parallel(y).vectorize(x, vec);

            // --- Main Fused Pipeline ---
            final_stage.compute_root()
                .reorder(c, x, y)
                .split(y, yo, yi, strip_size)
                .parallel(yo)
                .vectorize(x, vec);
            
            // Stages computed once per strip
            demosaiced.compute_at(final_stage, yo).vectorize(x, vec);
            deinterleaved.compute_at(final_stage, yo).vectorize(x, vec);
            
            // Stages computed per-scanline within each parallel strip.
            curved.compute_at(final_stage, yi).vectorize(x, vec);
            sharpened.compute_at(final_stage, yi).vectorize(x, vec);
            
            // --- Schedule Helpers ---
            corrected.compute_inline();
            for (Func f : color_correct_builder.intermediates) {
                f.compute_root();
            }

            auto& gaussian_kernel = sharpen_builder.intermediates[1];
            auto& luma_x = sharpen_builder.intermediates[2];
            auto& blurred_luma = sharpen_builder.intermediates[3];
            gaussian_kernel.compute_root();
            
            // SLIDING WINDOW OPTIMIZATION:
            luma_x.store_at(final_stage, yo).compute_at(final_stage, yi).vectorize(x, vec);
            blurred_luma.compute_at(sharpened, x).vectorize(x, vec);

            // Helpers for the root-computed CA stage
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos ||
                    f.name() == "g_interp" ||
                    f.name().find("norm_raw_ca") != std::string::npos) {
                    f.compute_root().parallel(f.args()[1]).vectorize(f.args()[0], vec);
                } else {
                    f.compute_at(ca_corrected, y).vectorize(x, vec);
                }
            }
        }
        
        // --- DEBUGGING ---
        // Uncomment the line below to generate an HTML file showing the final,
        // lowered pseudo-code for the denoising stage.
        /*
        final_stage.compile_to_lowered_stmt("final_stage_lowered.html",
            {input, demosaic_algorithm_id, matrix_3200, matrix_7000, color_temp,
             tint, sharpen_strength, sharpen_radius, sharpen_threshold, ca_correction_strength, denoise_strength,
             denoise_eps, blackLevel, whiteLevel, tone_curve_lut},
            HTML);
        */

        // Finally, assign the fully-defined and scheduled Func to the output.
        processed = final_stage;
        // The .bound() call should be on the final Func as well.
        final_stage.bound(c, 0, 3);
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
