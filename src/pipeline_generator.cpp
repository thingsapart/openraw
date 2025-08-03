#include "Halide.h"
#include "halide_trace_config.h"
#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>
#include <type_traits>

// Include the individual pipeline stages
#include "stage_hot_pixel_suppression.h"
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h" // Now the dispatcher
#include "stage_color_correct.h"
#include "stage_apply_curve.h"
#include "stage_sharpen.h"

using namespace Halide;

// Shared variables (moved outside anonymous namespace)
Var x("x"), y("y"), c("c"), yi("yi"), yo("yo"), yii("yii"), xi("xi");

template <typename T>
class CameraPipeGenerator : public Halide::Generator<CameraPipeGenerator<T>> {
public:
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
    typename Generator<CameraPipeGenerator<T>>::template Input<float> ca_correction_strength{"ca_correction_strength"};
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

        // Stage 0: Shift and convert input to the processing type
        Func raw_norm("raw_norm");
        Expr shifted_val = input(x + 16, y + 12);
        if (std::is_same<proc_type, float>::value) {
            // For float path, normalize to [0, 1]
            raw_norm(x, y) = (cast<float>(shifted_val) - blackLevel) / (whiteLevel - blackLevel);
        } else {
            // U16 FIX: Perform subtraction in a signed type and then saturate.
            // This prevents uint16 from wrapping around on negative results.
            raw_norm(x, y) = u16_sat(cast<int32_t>(shifted_val) - blackLevel);
        }

        // Stage 1: Hot pixel suppression
        Func denoised = pipeline_hot_pixel_suppression(raw_norm, x, y);
        
        // Stage 1.5: Chromatic Aberration Correction
        CACorrectBuilder_T<T> ca_builder{denoised, x, y,
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
        Func corrected = pipeline_color_correct(demosaiced, halide_proc_type, matrix_3200, matrix_7000,
                                                color_temp, tint, x, y, c, whiteLevel, blackLevel,
                                                this->get_target(), this->using_autoscheduler());

        // Stage 5: Apply tone curve from LUT
        Func tone_curve_func("tone_curve_func");
        Var lut_x("lut_x_var"), lut_c("lut_c_var");
        tone_curve_func(lut_x, lut_c) = tone_curve_lut(lut_x, lut_c);
        Expr lut_size_expr = tone_curve_lut.dim(0).extent();

        // Correctly call the templated function
        Func curved = pipeline_apply_curve<T>(corrected, blackLevel, whiteLevel,
                                           tone_curve_func, lut_size_expr, x, y, c,
                                           this->get_target(), this->using_autoscheduler());
        
        // Stage 6: Sharpen the image
        // Correctly call the templated function
        Func sharpened = pipeline_sharpen<T>(curved, sharpen_strength, x, y, c,
                                          this->get_target(), this->using_autoscheduler());

        // Final stage: Convert to 8-bit for output
        Expr final_val = sharpened(x, y, c);
        if (std::is_same<proc_type, float>::value) {
            processed(x, y, c) = u8_sat(final_val * 255.0f);
        } else {
            processed(x, y, c) = u8_sat(final_val >> 8);
        }

        processed.bound(c, 0, 3);

        // ========== ESTIMATES ==========
        input.set_estimates({{0, 2592}, {0, 1968}});
        demosaic_algorithm_id.set_estimate(3);
        matrix_3200.set_estimates({{0, 4}, {0, 3}});
        matrix_7000.set_estimates({{0, 4}, {0, 3}});
        tone_curve_lut.set_estimates({{0, 65536}, {0, 3}});
        color_temp.set_estimate(3700);
        tint.set_estimate(0.0f);
        sharpen_strength.set_estimate(1.0f);
        ca_correction_strength.set_estimate(1.0f);
        blackLevel.set_estimate(25);
        whiteLevel.set_estimate(1023);
        processed.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});

        // ========== SCHEDULE ==========
        if (this->using_autoscheduler()) {
            // Let the auto-scheduler do its thing.
        } else if (this->get_target().has_gpu_feature()) {
            // Manual GPU schedule
            Var xi, yi;
            int tile_x = 28, tile_y = 12;

            processed.compute_root().reorder(c, x, y).gpu_tile(x, y, xi, yi, tile_x, tile_y);
            curved.compute_at(processed, x).gpu_threads(x, y);
            corrected.compute_at(processed, x).gpu_threads(x, y);
            demosaiced.compute_at(processed, x).gpu_threads(x, y);
            
            for (Func f : demosaic_dispatcher.all_intermediates) {
                if (f.dimensions() >= 2) {
                    f.compute_at(processed, x).gpu_threads(f.args()[0], f.args()[1]);
                }
            }
            deinterleaved.compute_at(processed, x).unroll(x, 2).gpu_threads(x, y).reorder(c, x, y).unroll(c);
            ca_corrected.compute_at(processed, x).gpu_threads(x, y);
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name().find("norm_raw") != std::string::npos) {
                    f.compute_root().gpu_tile(f.args()[0], f.args()[1], xi, yi, 16, 16);
                } else {
                    f.compute_at(processed, x).gpu_threads(x,y);
                }
            }
            denoised.compute_root().gpu_tile(x, y, xi, yi, 16, 8);

        } else {
            // Manual CPU schedule
            Expr strip_size = 32;
            int vec = this->get_target().natural_vector_size(halide_proc_type);

            processed.compute_root()
                .reorder(c, x, y)
                .split(y, yo, yi, strip_size)
                .parallel(yo)
                .vectorize(x, vec);

            curved.compute_at(processed, yi)
                .store_at(processed, yo)
                .reorder(c, x, y)
                .vectorize(x, vec);
            
            corrected.compute_at(curved, x)
                .reorder(c, x, y)
                .vectorize(x)
                .unroll(c);
            
            demosaiced.compute_at(curved, x)
                .vectorize(x)
                .reorder(c, x, y)
                .unroll(c);

            deinterleaved.compute_at(processed, yi)
                .store_at(processed, yo)
                .reorder(c, x, y)
                .vectorize(x, vec);

            ca_corrected.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name().find("norm_raw") != std::string::npos) {
                    f.compute_root().parallel(f.args()[1]).vectorize(f.args()[0], vec);
                } else {
                    f.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);
                }
            }
            denoised.compute_root().parallel(y).vectorize(x, vec);
        }
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
