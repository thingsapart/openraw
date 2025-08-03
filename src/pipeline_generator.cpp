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
    // All internal stages will be uint16. The final output is uint8.
    // The result_type param is no longer needed as the pipeline is now fixed type.

    // --- Inputs ---
    Input<Buffer<uint16_t, 2>> input{"input"};
    Input<Buffer<float, 2>> matrix_3200{"matrix_3200"};
    Input<Buffer<float, 2>> matrix_7000{"matrix_7000"};
    Input<float> color_temp{"color_temp"};
    Input<float> tint{"tint"};
    Input<float> sharpen_strength{"sharpen_strength"};
    Input<float> ca_correction_strength{"ca_correction_strength"};
    Input<int> blackLevel{"blackLevel"};
    Input<int> whiteLevel{"whiteLevel"};
    // NEW: The tone curve LUT, replacing gamma and contrast.
    Input<Buffer<uint16_t, 2>> tone_curve_lut{"tone_curve_lut"};

    // --- Output ---
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
                                    blackLevel, whiteLevel,
                                    out_width_est, out_height_est,
                                    get_target(), using_autoscheduler());
        Func ca_corrected = ca_builder.output;

        // Stage 2: Deinterleave the Bayer pattern
        Func deinterleaved = pipeline_deinterleave(ca_corrected, x, y, c);

        // Stage 3: Demosaic
        DemosaicBuilder demosaic_builder(deinterleaved, x, y, c);
        Func demosaiced = demosaic_builder.output; // Output is int16

        // Stage 4: Color correction
        Func corrected = pipeline_color_correct(demosaiced, matrix_3200, matrix_7000,
                                                color_temp, tint, x, y, c,
                                                get_target(), using_autoscheduler()); // Output is int16

        // Stage 5: Apply tone curve from LUT
        // Wrap the Input LUT buffer in a Func to pass it to the stage helper.
        Func tone_curve_func("tone_curve_func");
        Var lut_x("lut_x_var"), lut_c("lut_c_var");
        tone_curve_func(lut_x, lut_c) = tone_curve_lut(lut_x, lut_c);

        Expr lut_size_expr = tone_curve_lut.dim(0).extent();
        Func curved = pipeline_apply_curve(corrected, blackLevel, whiteLevel,
                                           tone_curve_func, lut_size_expr, x, y, c,
                                           get_target(), using_autoscheduler()); // Output is uint16

        // Stage 6: Sharpen the image
        // This stage is modified to work with uint16 data.
        Func sharpened = pipeline_sharpen(curved, sharpen_strength, x, y, c,
                                          get_target(), using_autoscheduler()); // Output is uint16

        // Final stage: Convert to 8-bit for output
        processed(x, y, c) = cast<uint8_t>(sharpened(x, y, c) >> 8);


        // ========== ESTIMATES ==========
        // (This can be useful in conjunction with RunGen and benchmarks as well
        // as auto-schedule, so we do it in all cases.)
        input.set_estimates({{0, 2592}, {0, 1968}});
        matrix_3200.set_estimates({{0, 4}, {0, 3}});
        matrix_7000.set_estimates({{0, 4}, {0, 3}});
        tone_curve_lut.set_estimates({{0, 65536}, {0, 3}});
        color_temp.set_estimate(3700);
        tint.set_estimate(0.0f);
        sharpen_strength.set_estimate(1.0);
        ca_correction_strength.set_estimate(1.0);
        blackLevel.set_estimate(25);
        whiteLevel.set_estimate(1023);
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
                .gpu_tile(x, y, xi, yi, tile_x, tile_y);

            // Note: the `sharpen` stage is fused into `processed` by default.

            curved.compute_at(processed, x)
                .gpu_threads(x, y);

            corrected.compute_at(processed, x)
                .gpu_threads(x, y);

            demosaiced.compute_at(processed, x)
                .gpu_threads(x, y);
            for (Func f : demosaic_builder.intermediates) {
                f.compute_at(processed, x).gpu_threads(demosaic_builder.qx, demosaic_builder.qy);
            }

            deinterleaved.compute_at(processed, x)
                .unroll(x, 2)
                .gpu_threads(x, y)
                .reorder(c, x, y)
                .unroll(c);
            
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

            int vec = get_target().natural_vector_size(UInt(16));
            if (get_target().has_feature(Target::HVX)) {
                vec = 64;
            }

            processed.compute_root()
                .reorder(c, x, y)
                .split(y, yi, yii, 2, TailStrategy::RoundUp)
                .split(yi, yo, yi, strip_size / 2)
                .vectorize(x, vec, TailStrategy::RoundUp)
                .unroll(c)
                .parallel(yo);

            // Note: the `sharpen` stage is fused into `processed` by default.

            curved.compute_at(processed, yi)
                .store_at(processed, yo)
                .reorder(c, x, y)
                .tile(x, y, x, y, xi, yi, vec, 2, TailStrategy::RoundUp)
                .vectorize(xi)
                .unroll(yi)
                .unroll(c);
            
            corrected.compute_at(curved, x)
                .reorder(c, x, y)
                .vectorize(x)
                .unroll(c);

            demosaiced.compute_at(curved, x)
                .vectorize(x)
                .reorder(c, x, y)
                .unroll(c);
            
            Var demosaic_intermediate_v = demosaic_builder.qx;
            for (Func f : demosaic_builder.intermediates) {
                 f.compute_at(processed, yi)
                    .store_at(processed, yo)
                    .vectorize(demosaic_intermediate_v, vec, TailStrategy::RoundUp)
                    .fold_storage(demosaic_builder.qy, 4);
            }
            if (demosaic_builder.intermediates.size() > 1) {
                demosaic_builder.intermediates[1].compute_with(
                    demosaic_builder.intermediates[0], demosaic_intermediate_v,
                    {{demosaic_builder.qx, LoopAlignStrategy::AlignStart}, {demosaic_builder.qy, LoopAlignStrategy::AlignStart}});
            }

            deinterleaved.compute_at(processed, yi)
                .store_at(processed, yo)
                .fold_storage(y, 4)
                .reorder(c, x, y)
                .vectorize(x, vec, TailStrategy::RoundUp)
                .unroll(c);

            ca_corrected.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);
            for (Func f : ca_builder.intermediates) {
                if (f.name().find("shifts") != std::string::npos || f.name() == "g_interp" || f.name() == "norm_raw") {
                    f.compute_root().parallel(f.args()[1]).vectorize(f.args()[0], vec);
                } else {
                    f.compute_at(processed, yi).store_at(processed, yo).vectorize(x, vec);
                }
            }

            denoised.compute_root().parallel(y).vectorize(x, vec);

            if (get_target().has_feature(Target::HVX)) {
                processed.hexagon();
                denoised.align_storage(x, vec);
                ca_corrected.align_storage(x, vec);
                deinterleaved.align_storage(x, vec);
                corrected.align_storage(x, vec);
                curved.align_storage(x, vec);
            }

            processed
                .bound(c, 0, 3)
                .bound(x, 0, (out_width / vec) * vec)
                .bound(y, 0, (out_height / strip_size) * strip_size);


            // ========== TRACE TAGS for HalideTraceViz ==========
            // Visualization max values updated for uint16_t pipeline
            {
                Halide::Trace::FuncConfig cfg;
                cfg.max = 4095;
                cfg.pos = {10, 348};
                cfg.labels = {{"input"}};
                input.add_trace_tag(cfg.to_trace_tag());

                cfg.pos = {305, 360};
                cfg.labels = {{"denoised"}};
                denoised.add_trace_tag(cfg.to_trace_tag());
                
                cfg.pos = {580, 120};
                const int y_offset = 220;
                cfg.strides = {{1, 0}, {0, 1}, {0, y_offset}};
                cfg.labels = {
                    {"gr", {0, 0 * y_offset}},
                    {"r", {0, 1 * y_offset}},
                    {"b", {0, 2 * y_offset}},
                    {"gb", {0, 3 * y_offset}},
                };
                deinterleaved.add_trace_tag(cfg.to_trace_tag());
                
                cfg.pos = {860, 340 - 220};
                for(Func f : demosaic_builder.intermediates) {
                    std::string label = f.name();
                    std::replace(label.begin(), label.end(), '_', '@');
                    cfg.pos.y += 220;
                    cfg.labels = {{label}};
                    f.add_trace_tag(cfg.to_trace_tag());
                }

                cfg.color_dim = 2;
                cfg.strides = {{1, 0}, {0, 1}, {0, 0}};
                cfg.pos = {1140, 360};
                cfg.labels = {{"demosaiced"}};
                demosaiced.add_trace_tag(cfg.to_trace_tag());

                cfg.pos = {1400, 360};
                cfg.labels = {{"color-corrected"}};
                corrected.add_trace_tag(cfg.to_trace_tag());

                cfg.max = 65535;
                cfg.pos = {1660, 360};
                cfg.labels = {{"tonemapped"}};
                curved.add_trace_tag(cfg.to_trace_tag());
                
                cfg.pos = {1920, 360};
                cfg.labels = {{"sharpened"}};
                sharpened.add_trace_tag(cfg.to_trace_tag());

                cfg.max = 255;
                cfg.pos = {2180, 360};
                cfg.labels = {{"final_output"}};
                processed.add_trace_tag(cfg.to_trace_tag());
            }
        }
    }
};

}  // namespace

HALIDE_REGISTER_GENERATOR(CameraPipe, camera_pipe)
