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
#include "stage_exposure.h"
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
    Input<Buffer<float, 2>> matrix_3200{"matrix_3200"};
    Input<Buffer<float, 2>> matrix_7000{"matrix_7000"};
    Input<float> color_temp{"color_temp"};
    Input<float> tint{"tint"};
    Input<float> exposure{"exposure"};
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

        // --- PIPELINE CONVERSION TO FLOAT32 ---
        // Convert to Float32 to prevent integer overflow/underflow in demosaic
        // and to preserve precision in subsequent color-related stages.
        // We do not normalize here; stages will operate on a [0, 65535] float range.
        Func to_float("to_float");
        to_float(x, y) = cast<float>(ca_corrected(x, y));

        // Stage 2: Deinterleave the Bayer pattern
        Func deinterleaved = pipeline_deinterleave(to_float, x, y, c);

        // Stage 3: Demosaic. Now operates on Float32 data.
        DemosaicBuilder demosaic_builder(deinterleaved, x, y, c, demosaic_algorithm, out_width_est / 2, out_height_est / 2);
        Func demosaiced = demosaic_builder.output;

        // Stage 4: Color correction. Must be done before exposure.
        Func color_corrected = pipeline_color_correct(demosaiced, matrix_3200, matrix_7000,
                                                      color_temp, tint, x, y, c,
                                                      get_target(), using_autoscheduler());
        
        // Stage 5: Exposure adjustment. Applied to linear RGB data.
        Func exposed = pipeline_exposure(color_corrected, exposure, x, y, c);

        // Stage 6: Saturation adjustment
        Func saturated = pipeline_saturation(exposed, saturation, saturation_algorithm, x, y, c);

        // --- PIPELINE CONVERSION BACK TO UINT16 ---
        // Convert back to uint16 for the curves and sharpening stages.
        Func to_uint16("to_uint16");
        to_uint16(x, y, c) = cast<uint16_t>(clamp(saturated(x, y, c), 0.0f, 65535.0f));

        // Stage 7: Apply tone curve
        // Get the LUT extent from the Input buffer and pass it to the helper.
        Expr lut_extent = tone_curves.dim(0).extent();
        Func curved = pipeline_apply_curve(to_uint16, tone_curves, lut_extent, curve_mode, x, y, c);

        // Stage 8: Sharpen the image
        Func sharpened = pipeline_sharpen(curved, sharpen_strength, x, y, c,
                                          get_target(), using_autoscheduler());

        // Stage 9: Convert from 16-bit to 8-bit using a selectable tone mapping operator.
        Func normalized16("normalized16");
        normalized16(x, y, c) = cast<float>(sharpened(x, y, c)) / 65535.0f;
        
        // Algorithm 0: Linear mapping (fastest, clips highlights)
        Func linear_tonemapped("linear_tonemapped");
        linear_tonemapped(x, y, c) = u8_sat(sharpened(x, y, c) >> 8);
        
        // Algorithm 1: Reinhard tone mapping (good highlight rolloff)
        Func reinhard_tonemapped("reinhard_tonemapped");
        {
            Expr reinhard_val = normalized16(x, y, c) / (normalized16(x, y, c) + 1.0f);
            // Apply a gamma curve for contrast restoration. A gamma of 1.5 darkens midtones.
            const float contrast_gamma = 1.0f / 1.5f;
            Expr contrast_restored = pow(reinhard_val, contrast_gamma);
            reinhard_tonemapped(x, y, c) = u8_sat(contrast_restored * 255.0f);
        }

        // Algorithm 2: Filmic S-curve (cinematic, protects highlights and shadows)
        Func filmic_tonemapped("filmic_tonemapped");
        {
            // This curve is defined for an input with an exposure bias.
            const float exposure_bias = 2.0f;
            Expr biased_input = normalized16(x, y, c) * exposure_bias;
            
            // The "white point" W defines the linear value that maps to white after the curve.
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
        // (This can be useful in conjunction with RunGen and benchmarks as well
        // as auto-schedule, so we do it in all cases.)
        input.set_estimates({{0, 2592}, {0, 1968}});
        matrix_3200.set_estimates({{0, 4}, {0, 3}});
        matrix_7000.set_estimates({{0, 4}, {0, 3}});
        color_temp.set_estimate(3700);
        tint.set_estimate(0.0f);
        exposure.set_estimate(1.0f);
        saturation.set_estimate(1.0f);
        saturation_algorithm.set_estimate(1); // Default to L*a*b*
        demosaic_algorithm.set_estimate(0); // Default to simple
        curve_mode.set_estimate(1); // Default to RGB
        tone_curves.set_estimates({{0, 65536}, {0, 3}});
        sharpen_strength.set_estimate(1.0);
        ca_correction_strength.set_estimate(1.0);
        tonemap_algorithm.set_estimate(0); // Default to linear
        processed.set_estimates({{0, out_width_est}, {0, out_height_est}, {0, 3}});


        // ========== SCHEDULE ==========
        if (using_autoscheduler()) {
            // Let the auto-scheduler do its thing.
            // The estimates above will be used.
        } else if (get_target().has_gpu_feature()) {
            // Manual GPU schedule
            Expr out_width = processed.width();
            Expr out_height = processed.height();
            processed.bound(c, 0, 3)
                .bound(x, 0, (out_width / 2) * 2)
                .bound(y, 0, (out_height / 2) * 2);

            Var xi, yi;

            int tile_x = 28;
            int tile_y = 12;
            if (get_target().has_feature(Target::D3D12Compute)) {
                tile_x = 20;
                tile_y = 12;
            }

            processed.compute_root()
                .reorder(c, x, y)
                .unroll(x, 2)
                .gpu_tile(x, y, xi, yi, tile_x, tile_y);
            
            final_output.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y);

            // Note: the `sharpen` stage is fused into `final_output` by default.

            curved.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y);

            to_uint16.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y);

            saturated.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y);

            exposed.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y);

            color_corrected.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y);

            demosaiced.compute_at(processed, x).gpu_threads(x, y);
            demosaic_builder.simple_output.compute_at(processed, x).gpu_threads(x, y);
            demosaic_builder.vhg_output.compute_at(processed, x).gpu_threads(x, y);
            for (Func f : demosaic_builder.quarter_res_intermediates) {
                f.compute_at(processed, x).gpu_threads(demosaic_builder.qx, demosaic_builder.qy);
            }

            deinterleaved.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y)
                .reorder(c, x, y)
                .unroll(c);

            to_float.compute_at(processed, x).gpu_threads(x,y);

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

            Expr strip_size;
            if (get_target().has_feature(Target::HVX)) {
                strip_size = processed.dim(1).extent() / 4;
            } else {
                strip_size = 32;
            }
            strip_size = (strip_size / 2) * 2;

            int vec = get_target().natural_vector_size(Float(32));
            if (get_target().has_feature(Target::HVX)) {
                vec = 32; // for float
            }

            processed.compute_root()
                .reorder(c, x, y)
                .split(y, yi, yii, 2, TailStrategy::RoundUp)
                .split(yi, yo, yi, strip_size / 2)
                .vectorize(x, 2 * vec, TailStrategy::RoundUp)
                .unroll(c)
                .parallel(yo);
            
            final_output.compute_at(processed, yi)
                .store_at(processed, yo)
                .reorder(c, x, y)
                .tile(x, y, x, y, xi, yi, 2 * vec, 2, TailStrategy::RoundUp)
                .vectorize(xi)
                .unroll(yi)
                .unroll(c);

            // Note: the `sharpen` stage is fused into `final_output` by default.

            curved.compute_at(processed, yi)
                .store_at(processed, yo)
                .reorder(c, x, y)
                .tile(x, y, x, y, xi, yi, get_target().natural_vector_size(UInt(16)), 2, TailStrategy::RoundUp)
                .vectorize(xi)
                .unroll(yi)
                .unroll(c);
            
            to_uint16.compute_at(curved, x).vectorize(x).unroll(c);
            saturated.compute_at(curved, x).vectorize(x).unroll(c);
            exposed.compute_at(curved, x).vectorize(x).unroll(c);
            color_corrected.compute_at(curved, x).vectorize(x).unroll(c);

            demosaiced.compute_at(curved, x).vectorize(x).unroll(c);
            demosaic_builder.simple_output.compute_at(curved, x).vectorize(x).unroll(c);
            demosaic_builder.vhg_output.compute_at(curved, x).vectorize(x).unroll(c);

            Var demosaic_intermediate_v = demosaic_builder.qx;
            for (Func f : demosaic_builder.quarter_res_intermediates) {
                 f.compute_at(processed, yi)
                    .store_at(processed, yo)
                    .vectorize(demosaic_intermediate_v, vec, TailStrategy::RoundUp)
                    .fold_storage(demosaic_builder.qy, 4);
            }
            if (demosaic_builder.quarter_res_intermediates.size() > 1) {
                demosaic_builder.quarter_res_intermediates[1].compute_with(
                    demosaic_builder.quarter_res_intermediates[0], demosaic_intermediate_v,
                    {{demosaic_builder.qx, LoopAlignStrategy::AlignStart}, {demosaic_builder.qy, LoopAlignStrategy::AlignStart}});
            }

            deinterleaved.compute_at(processed, yi)
                .store_at(processed, yo)
                .fold_storage(y, 4)
                .reorder(c, x, y)
                .vectorize(x, vec, TailStrategy::RoundUp)
                .unroll(c);
            
            to_float.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);

            ca_corrected.compute_at(processed, yi).store_at(processed, yo).vectorize(x, get_target().natural_vector_size(UInt(16)));
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name() == "norm_raw") {
                    f.compute_root().parallel(f.args()[1]).vectorize(f.args()[0], vec);
                } else {
                    f.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);
                }
            }

            denoised.compute_root().parallel(y).vectorize(x, get_target().natural_vector_size(UInt(16)) * 2);

            if (get_target().has_feature(Target::HVX)) {
                processed.hexagon();
                denoised.align_storage(x, 128);
                ca_corrected.align_storage(x, 128);
                to_float.align_storage(x, 128);
                deinterleaved.align_storage(x, 128);
                exposed.align_storage(x, 128);
                color_corrected.align_storage(x, 128);
                saturated.align_storage(x, 128);
                to_uint16.align_storage(x, 128);
            }

            processed
                .bound(c, 0, 3)
                .bound(x, 0, ((out_width) / (2 * vec)) * (2 * vec))
                .bound(y, 0, (out_height / strip_size) * strip_size);


            // ========== TRACE TAGS for HalideTraceViz ==========
            {
                // Visualization is complex now that the LUT is pre-generated.
                // We'll simplify the trace tags for now.
            }
        }
    }
};

}  // namespace

HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
