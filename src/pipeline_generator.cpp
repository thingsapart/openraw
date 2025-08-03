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
#include "stage_demosaic.h"
#include "stage_normalize_and_expose.h"
#include "stage_color_correct.h"
#include "stage_saturation.h"
#include "stage_apply_curve.h" // This is now the final tonemap stage
#include "stage_sharpen.h"

namespace {

using namespace Halide;
using namespace Halide::ConciseCasts;

// Shared variables
Var x("x"), y("y"), c("c"), yi("yi"), yo("yo"), yii("yii"), xi("xi");


class CameraPipe : public Halide::Generator<CameraPipe> {
public:
    Input<Buffer<uint16_t, 2>> input{"input"};
    Input<float> black_point{"black_point"};
    Input<float> white_point{"white_point"};
    Input<float> exposure{"exposure"};
    Input<Buffer<float, 2>> matrix_3200{"matrix_3200"};
    Input<Buffer<float, 2>> matrix_7000{"matrix_7000"};
    Input<float> color_temp{"color_temp"};
    Input<float> tint{"tint"};
    Input<float> saturation{"saturation"};
    Input<int> saturation_algorithm{"saturation_algorithm"};
    Input<int> demosaic_algorithm{"demosaic_algorithm"};
    Input<int> curve_mode{"curve_mode"};
    // FIX: The LUT is now uint8, as it contains the final 8-bit values.
    Input<Buffer<uint8_t, 2>> tone_curves{"tone_curves"};
    Input<float> sharpen_strength{"sharpen_strength"};
    Input<float> ca_correction_strength{"ca_correction_strength"};
    // Note: tonemap_algorithm is now passed to the host-side LUT generator, not Halide.
    Output<Buffer<uint8_t, 3>> processed{"processed"};

    void generate() {
        // ========== THE ALGORITHM ==========
        Expr out_width_est = 2592 - 32;
        Expr out_height_est = 1968 - 24;

        // Stage 0: Shift input
        Func shifted("shifted");
        shifted(x, y) = input(x + 16, y + 12);

        // Stage 1: Hot pixel suppression
        Func denoised = pipeline_hot_pixel_suppression(shifted, x, y);

        // Stage 1.5: Chromatic Aberration Correction
        CACorrectBuilder ca_builder(denoised, x, y,
                                    ca_correction_strength,
                                    1, 4095,
                                    out_width_est, out_height_est,
                                    get_target(), using_autoscheduler());
        Func ca_corrected = ca_builder.output;

        // Stage 1.7: Normalization and Exposure to Float32
        Func normalized = pipeline_normalize_and_expose(ca_corrected, black_point, white_point, exposure, x, y);

        // Stage 2: Deinterleave
        Func deinterleaved = pipeline_deinterleave(normalized, x, y, c);

        // Stage 3: Demosaic
        DemosaicBuilder demosaic_builder(deinterleaved, x, y, c, demosaic_algorithm, out_width_est, out_height_est);
        Func demosaiced = demosaic_builder.output;

        // Stage 4: Color correction
        Func color_corrected = pipeline_color_correct(demosaiced, matrix_3200, matrix_7000,
                                                      color_temp, tint, white_point, x, y, c,
                                                      get_target(), using_autoscheduler());

        // Stage 5: Saturation
        Func saturated = pipeline_saturation(color_corrected, saturation, saturation_algorithm, x, y, c);

        // OPTIMIZATION: Sharpening is now done in linear float space for better quality.
        // We declare the intermediate blur Funcs here so they can be scheduled.
        Func unsharp("unsharp"), unsharp_y("unsharp_y");
        Func sharpened = pipeline_sharpen(saturated, sharpen_strength, x, y, c, unsharp, unsharp_y);

        // OPTIMIZATION: Final tonemapping is now a single LUT lookup.
        // The complex math is pre-calculated on the host.
        // First, convert the linear float data to a uint16 index for the LUT.
        Func to_uint16_for_lut("to_uint16_for_lut");
        to_uint16_for_lut(x, y, c) = cast<uint16_t>(clamp(sharpened(x, y, c) * 65535.0f, 0.0f, 65535.0f));

        // The LUT now contains the final 8-bit values.
        Expr lut_extent = tone_curves.dim(0).extent();
        Func curved = pipeline_apply_curve(to_uint16_for_lut, tone_curves, lut_extent, curve_mode, x, y, c);

        // Set the final output Func
        processed(x, y, c) = curved(x, y, c);

        // ========== ESTIMATES ==========
        input.set_estimates({{0, 2592}, {0, 1968}});
        black_point.set_estimate(25.0f);
        white_point.set_estimate(4095.0f);
        exposure.set_estimate(1.0f);
        matrix_3200.set_estimates({{0, 4}, {0, 3}});
        matrix_7000.set_estimates({{0, 4}, {0, 3}});
        color_temp.set_estimate(3700);
        tint.set_estimate(0.0f);
        saturation.set_estimate(1.0f);
        saturation_algorithm.set_estimate(1);
        demosaic_algorithm.set_estimate(1);
        curve_mode.set_estimate(1);
        tone_curves.set_estimates({{0, 65536}, {0, 3}});
        sharpen_strength.set_estimate(1.0);
        ca_correction_strength.set_estimate(1.0);
        processed.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});


        // ========== SCHEDULE ==========
        if (using_autoscheduler()) {
            // Let the auto-scheduler do its thing.
        } else if (get_target().has_gpu_feature()) {
            // Manual GPU schedule
            Var xi, yi;
            processed.compute_root().gpu_tile(x, y, xi, yi, 28, 12);
            curved.compute_at(processed, xi).gpu_threads(xi, yi);
            to_uint16_for_lut.compute_at(processed, xi).gpu_threads(xi, yi);
            sharpened.compute_at(processed, xi).gpu_threads(xi, yi);
            unsharp.compute_at(processed, xi).gpu_threads(xi, yi);
            unsharp_y.compute_at(processed, xi).gpu_threads(xi, yi);
            saturated.compute_at(processed, xi).gpu_threads(xi, yi);
            color_corrected.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaiced.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.simple_output.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.vhg_output.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.ahd_output.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.amaze_output.compute_at(processed, xi).gpu_threads(xi, yi);
            for (Func f : demosaic_builder.quarter_res_intermediates) {
                f.compute_at(processed, x);
            }
            for (Func f : demosaic_builder.full_res_intermediates) {
                f.compute_at(processed, xi).gpu_threads(xi, yi);
            }
            deinterleaved.compute_at(processed, xi).gpu_threads(xi, yi).unroll(c);
            normalized.compute_at(processed, xi).gpu_threads(xi, yi);
            ca_corrected.compute_at(processed, xi).gpu_threads(xi, yi);
            denoised.compute_root().gpu_tile(x, y, xi, yi, 16, 8);

        } else {
            // Manual CPU schedule
            int vec = get_target().natural_vector_size(Float(32));
            Expr strip_size = 32;

            processed.compute_root()
                .split(y, yo, yi, strip_size)
                .parallel(yo)
                .vectorize(x, vec * 2);

            // Stages are computed per strip, from output backwards.
            curved.compute_at(processed, yi).vectorize(x, vec * 2);
            to_uint16_for_lut.compute_at(processed, yi).vectorize(x, vec);
            sharpened.compute_at(processed, yi).vectorize(x, vec);

            // OPTIMIZATION: Schedule intermediate sharpening blurs to avoid recomputation.
            unsharp_y.compute_at(processed, yi).vectorize(x, vec);
            unsharp.compute_at(processed, yi).vectorize(x, vec);
            
            saturated.compute_at(processed, yi).vectorize(x, vec);
            color_corrected.compute_at(processed, yi).vectorize(x, vec);
            demosaiced.compute_at(processed, yi).vectorize(x, vec);
            
            demosaic_builder.simple_output.compute_at(processed, yi).vectorize(x, vec);
            demosaic_builder.vhg_output.compute_at(processed, yi).vectorize(x, vec);
            demosaic_builder.ahd_output.compute_at(processed, yi).vectorize(x, vec);
            demosaic_builder.amaze_output.compute_at(processed, yi).vectorize(x, vec);
            
            Var demosaic_intermediate_v = demosaic_builder.qx;
            for (Func f : demosaic_builder.quarter_res_intermediates) {
                 f.compute_at(processed, yi).vectorize(demosaic_intermediate_v, vec);
            }
            for (Func f : demosaic_builder.full_res_intermediates) {
                f.compute_at(processed, yi).vectorize(x, vec);
            }

            deinterleaved.compute_at(processed, yi).vectorize(x, vec);
            normalized.compute_at(processed, yi).vectorize(x, vec);
            ca_corrected.compute_at(processed, yi).vectorize(x, get_target().natural_vector_size(UInt(16)));
            
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name() == "norm_raw") {
                    f.compute_root().vectorize(f.args()[0], vec);
                }
            }
            denoised.compute_root().vectorize(x, get_target().natural_vector_size(UInt(16)) * 2);
        }
    }
};

}  // namespace

HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
