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
#include "stage_normalize_and_expose.h" // New, combined stage
#include "stage_color_correct.h"
#include "stage_saturation.h"
#include "stage_apply_curve.h"
#include "stage_sharpen.h"

namespace {

using namespace Halide;
using namespace Halide::ConciseCasts;

// Shared variables
Var x("x"), y("y"), c("c"), yi("yi"), yo("yo"), yii("yii"), xi("xi");


// A helper function to implement the Uncharted 2 filmic tone mapping curve.
// Source: http://filmicworlds.com/blog/filmic-tonemapping-operators/
Expr uncharted2_tonemap_partial(Expr x) {
    const float A = 0.22f; // Shoulder strength
    const float B = 0.30f; // Linear strength
    const float C = 0.10f; // Linear angle
    const float D = 0.20f; // Toe strength
    const float E = 0.01f; // Toe numerator
    const float F = 0.30f; // Toe denominator
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}


class CameraPipe : public Halide::Generator<CameraPipe> {
public:
    // Parameterized output type, because LLVM PTX (GPU) backend does not
    // currently allow 8-bit computations
    GeneratorParam<Type> result_type{"result_type", UInt(8)};

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
    Input<Buffer<uint16_t, 2>> tone_curves{"tone_curves"};
    Input<float> sharpen_strength{"sharpen_strength"};
    Input<float> ca_correction_strength{"ca_correction_strength"};
    Input<int> tonemap_algorithm{"tonemap_algorithm"};
    Output<Buffer<uint8_t, 3>> processed{"processed"};

    void generate() {
        // ========== THE ALGORITHM ==========
        // A sequence of Funcs defining the camera pipeline.
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
                                    1, 4095, // Use a 12-bit range
                                    out_width_est, out_height_est,
                                    get_target(), using_autoscheduler());
        Func ca_corrected = ca_builder.output;

        // Stage 1.7: Normalization and Exposure
        // This is the correct place to do black subtraction and white point scaling.
        // It converts the uint16 raw data to float and normalizes it to a [0, N] range.
        Func normalized = pipeline_normalize_and_expose(ca_corrected, black_point, white_point, exposure, x, y);

        // Stage 2: Deinterleave the Bayer pattern
        Func deinterleaved = pipeline_deinterleave(normalized, x, y, c);

        // Stage 3: Demosaic. Now operates on normalized Float32 data.
        DemosaicBuilder demosaic_builder(deinterleaved, x, y, c, demosaic_algorithm, out_width_est, out_height_est);
        Func demosaiced = demosaic_builder.output;

        // Stage 4: Color correction.
        Func color_corrected = pipeline_color_correct(demosaiced, matrix_3200, matrix_7000,
                                                      color_temp, tint, x, y, c,
                                                      get_target(), using_autoscheduler());

        // Stage 5: Saturation adjustment (operates on linear float data)
        Func saturated = pipeline_saturation(color_corrected, saturation, saturation_algorithm, x, y, c);

        // Stage 6: Apply tone curve
        // The input is now normalized float. We convert it to a uint16 index for the LUT.
        // The LUT now represents a pure gamma/S-curve mapping from a [0,1] domain.
        Func to_uint16_for_lut("to_uint16_for_lut");
        // We scale by 65535 to use the full range of the uint16 LUT.
        to_uint16_for_lut(x, y, c) = cast<uint16_t>(clamp(saturated(x, y, c) * 65535.0f, 0.0f, 65535.0f));

        Expr lut_extent = tone_curves.dim(0).extent();
        Func curved = pipeline_apply_curve(to_uint16_for_lut, tone_curves, lut_extent, curve_mode, x, y, c);

        // Stage 7: Sharpen the image
        Func sharpened = pipeline_sharpen(curved, sharpen_strength, x, y, c,
                                          get_target(), using_autoscheduler());

        // Stage 8: Convert from 16-bit to 8-bit using a selectable tone mapping operator.
        Func normalized16("normalized16");
        normalized16(x, y, c) = cast<float>(sharpened(x, y, c)) / 65535.0f;
        
        // Algorithm 0: Linear mapping (fastest, clips highlights)
        Func linear_tonemapped("linear_tonemapped");
        linear_tonemapped(x, y, c) = u8_sat(sharpened(x, y, c) >> 8);
        
        // Algorithm 1: Reinhard tone mapping (good highlight rolloff)
        Func reinhard_tonemapped("reinhard_tonemapped");
        {
            Expr reinhard_val = normalized16(x, y, c) / (normalized16(x, y, c) + 1.0f);
            const float contrast_gamma = 1.0f / 1.5f;
            Expr contrast_restored = pow(reinhard_val, contrast_gamma);
            reinhard_tonemapped(x, y, c) = u8_sat(contrast_restored * 255.0f);
        }

        // Algorithm 2: Filmic S-curve (cinematic, protects highlights and shadows)
        Func filmic_tonemapped("filmic_tonemapped");
        {
            const float exposure_bias = 2.0f;
            Expr biased_input = normalized16(x, y, c) * exposure_bias;
            const float W = 11.2f;
            Expr curve_val = uncharted2_tonemap_partial(biased_input);
            Expr white_scale = 1.0f / uncharted2_tonemap_partial(W);
            Expr final_val = curve_val * white_scale;
            filmic_tonemapped(x, y, c) = u8_sat(final_val * 255.0f);
        }
        
        Func final_output("final_output");
        final_output(x, y, c) = select(tonemap_algorithm == 2, filmic_tonemapped(x, y, c),
                                       tonemap_algorithm == 1, reinhard_tonemapped(x, y, c),
                                       /* else */              linear_tonemapped(x, y, c));

        // Set the final output Func
        processed(x, y, c) = final_output(x, y, c);


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
        tonemap_algorithm.set_estimate(0);
        processed.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});


        // ========== SCHEDULE ==========
        if (using_autoscheduler()) {
            // Let the auto-scheduler do its thing.
        } else if (get_target().has_gpu_feature()) {
            // Manual GPU schedule
            Var xi, yi;
            processed.compute_root().gpu_tile(x, y, xi, yi, 28, 12);
            final_output.compute_at(processed, xi).gpu_threads(xi, yi);
            curved.compute_at(processed, xi).gpu_threads(xi, yi);
            to_uint16_for_lut.compute_at(processed, xi).gpu_threads(xi, yi);
            saturated.compute_at(processed, xi).gpu_threads(xi, yi);
            color_corrected.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaiced.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.simple_output.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.vhg_output.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.ahd_output.compute_at(processed, xi).gpu_threads(xi, yi);
            demosaic_builder.lmmse_output.compute_at(processed, xi).gpu_threads(xi, yi);
            for (Func f : demosaic_builder.quarter_res_intermediates) {
                f.compute_at(processed, x); // Compute at block level
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

            // Corrected schedule order: split first, then parallelize.
            processed.compute_root()
                .split(y, yo, yi, strip_size)
                .parallel(yo)
                .vectorize(x, 2 * vec);

            final_output.compute_at(processed, yi).vectorize(x, 2 * vec);
            curved.compute_at(processed, yi).vectorize(x, get_target().natural_vector_size(UInt(16)));
            to_uint16_for_lut.compute_at(curved, x).vectorize(x, vec);
            saturated.compute_at(curved, x).vectorize(x, vec);
            color_corrected.compute_at(curved, x).vectorize(x, vec);

            demosaiced.compute_at(curved, x).vectorize(x, vec);
            demosaic_builder.simple_output.compute_at(curved, x).vectorize(x, vec);
            demosaic_builder.vhg_output.compute_at(curved, x).vectorize(x, vec);
            demosaic_builder.ahd_output.compute_at(curved, x).vectorize(x, vec);
            demosaic_builder.lmmse_output.compute_at(curved, x).vectorize(x, vec);
            
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
            
            // FIX: Remove nested parallelism from CA intermediates.
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name() == "norm_raw") {
                    f.compute_root().vectorize(f.args()[0], vec);
                } else {
                    f.compute_at(processed, yi).vectorize(x, vec);
                }
            }

            denoised.compute_root().vectorize(x, get_target().natural_vector_size(UInt(16)) * 2);
        }
    }
};

}  // namespace

HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
