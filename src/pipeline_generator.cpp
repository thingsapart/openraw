#include "Halide.h"
#include "halide_trace_config.h"
#include <stdint.h>
#include <string>
#include <algorithm>
#include <vector>

// Include the individual pipeline stages
#include "stage_hot_pixel_suppression.h"
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h" // Now the dispatcher
#include "stage_color_correct.h"
#include "stage_apply_curve.h"
#include "stage_sharpen.h"

namespace {

using namespace Halide;
using namespace Halide::ConciseCasts;

// Shared variables
Var x("x"), y("y"), c("c"), yi("yi"), yo("yo"), yii("yii"), xi("xi");

class CameraPipe : public Halide::Generator<CameraPipe> {
public:
    // --- Inputs ---
    Input<Buffer<uint16_t, 2>> input{"input"};
    Input<int> demosaic_algorithm_id{"demosaic_algorithm_id"};
    Input<Buffer<float, 2>> matrix_3200{"matrix_3200"};
    Input<Buffer<float, 2>> matrix_7000{"matrix_7000"};
    Input<float> color_temp{"color_temp"};
    Input<float> tint{"tint"};
    Input<float> sharpen_strength{"sharpen_strength"};
    Input<float> ca_correction_strength{"ca_correction_strength"};
    Input<int> blackLevel{"blackLevel"};
    Input<int> whiteLevel{"whiteLevel"};
    Input<Buffer<uint16_t, 2>> tone_curve_lut{"tone_curve_lut"};

    // --- Output ---
    Output<Buffer<uint8_t, 3>> processed{"processed"};

    void generate() {
        // ========== THE ALGORITHM ==========
        Expr out_width_est = 2592 - 32;
        Expr out_height_est = 1968 - 24;


        // Stage 0: Shift the input around to deal with boundary conditions
        Func shifted("shifted");
        shifted(x, y) = input(x + 16, y + 12);

        // Stage 1: Hot pixel suppression
        Func denoised = pipeline_hot_pixel_suppression(shifted, x, y);
        
        // Stage 1.5: Chromatic Aberration Correction
        CACorrectBuilder ca_builder(denoised, x, y,
                                    ca_correction_strength,
                                    blackLevel, whiteLevel,
                                    out_width_est, out_height_est,
                                    get_target(), using_autoscheduler());
        Func ca_corrected = ca_builder.output;

        // Stage 2: Deinterleave the Bayer pattern
        Func deinterleaved = pipeline_deinterleave(ca_corrected, x, y, c);

        // Stage 3: Demosaic using the dispatcher
        DemosaicDispatcher demosaic_dispatcher(deinterleaved, demosaic_algorithm_id, x, y, c);
        Func demosaiced = demosaic_dispatcher.output;

        // Stage 4: Color correction
        Func corrected = pipeline_color_correct(demosaiced, matrix_3200, matrix_7000,
                                                color_temp, tint, x, y, c,
                                                get_target(), using_autoscheduler());

        // Stage 5: Apply tone curve from LUT
        Func tone_curve_func("tone_curve_func");
        Var lut_x("lut_x_var"), lut_c("lut_c_var");
        tone_curve_func(lut_x, lut_c) = tone_curve_lut(lut_x, lut_c);

        Expr lut_size_expr = tone_curve_lut.dim(0).extent();
        Func curved = pipeline_apply_curve(corrected, blackLevel, whiteLevel,
                                           tone_curve_func, lut_size_expr, x, y, c,
                                           get_target(), using_autoscheduler());

        // Stage 6: Sharpen the image
        Func sharpened = pipeline_sharpen(curved, sharpen_strength, x, y, c,
                                          get_target(), using_autoscheduler());

        // Final stage: Convert to 8-bit for output
        processed(x, y, c) = cast<uint8_t>(sharpened(x, y, c) >> 8);

        // ***** PERFORMANCE FIX 1 *****
        // Bound the color channel dimension to a constant '3'. This allows
        // Halide to unroll loops over 'c' because it knows the extent at compile time.
        processed.bound(c, 0, 3);


        // ========== ESTIMATES ==========
        input.set_estimates({{0, 2592}, {0, 1968}});
        demosaic_algorithm_id.set_estimate(3); // Default to 'fast'
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
        if (using_autoscheduler()) {
            // Let the auto-scheduler do its thing.
        } else if (get_target().has_gpu_feature()) {
            // Manual GPU schedule
            Expr out_width = processed.width();
            Expr out_height = processed.height();
            // Note: processed.bound(c, 0, 3) is already set above

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
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name() == "norm_raw") {
                    f.compute_root().gpu_tile(f.args()[0], f.args()[1], xi, yi, 16, 16);
                } else {
                    f.compute_at(processed, x).gpu_threads(x,y);
                }
            }
            denoised.compute_root().gpu_tile(x, y, xi, yi, 16, 8);

        } else {
            // Manual CPU schedule
            Expr out_width = processed.width();
            Expr out_height = processed.height();

            Expr strip_size = 32;
            int vec = get_target().natural_vector_size(UInt(16));

            processed.compute_root()
                .reorder(c, x, y)
                .split(y, yo, yi, strip_size)
                .parallel(yo)
                .vectorize(x, vec);

            // `sharpened` is fused into `processed` by default (no schedule)

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

            // Demosaic intermediates are left unscheduled to be fused.
            
            // ***** PERFORMANCE FIX 2 *****
            // The .fold_storage() call has been removed. It was causing a
            // potential out-of-bounds read with the current scheduling strategy.
            deinterleaved.compute_at(processed, yi)
                .store_at(processed, yo)
                .reorder(c, x, y)
                .vectorize(x, vec);

            ca_corrected.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name() == "norm_raw") {
                    f.compute_root().parallel(f.args()[1]).vectorize(f.args()[0], vec);
                } else {
                    f.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);
                }
            }

            denoised.compute_root().parallel(y).vectorize(x, vec);
        }
    }
};

}  // namespace

HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
