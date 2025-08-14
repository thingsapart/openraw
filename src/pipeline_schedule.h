#ifndef PIPELINE_SCHEDULE_H
#define PIPELINE_SCHEDULE_H

#include "Halide.h"
#include "stage_ca_correct.h"
#include "stage_demosaic.h"
#include "stage_resize.h"
#include "stage_local_adjust_laplacian.h"
#include "stage_color_correct.h"

// This schedule function encapsulates the complex scheduling logic for the
// Laplacian-based pipeline. It is called from the main generator.
template <typename P> // P is the processing type (e.g. float, uint16_t)
void schedule_pipeline(
    bool is_autoscheduled,
    const Halide::Target& target,
    Halide::Func denoised,
    CACorrectBuilder& ca_builder,
    Halide::Func deinterleaved_hi_fi,
    Halide::Func demosaiced,
    DemosaicDispatcherT<float>& demosaic_dispatcher,
    Halide::Func downscaled,
    Halide::Expr is_no_op,
    ResizeBicubicBuilder& resize_builder,
    Halide::Func corrected_hi_fi,
    Halide::Func sharpened,
    LocalLaplacianBuilder& local_laplacian_builder,
    Halide::Func curved,
    Halide::Func final_stage,
    ColorCorrectBuilder_T<P>& color_correct_builder,
    Halide::Func tone_curve_func,
    Halide::Type halide_proc_type,
    Halide::Var x, Halide::Var y, Halide::Var c, Halide::Var xo, Halide::Var xi, Halide::Var yo, Halide::Var yi,
    int J, int cutover_level)
{
    using namespace Halide;

    if (is_autoscheduled) {
        // Autoscheduler will determine the schedule.
    } else if (target.has_gpu_feature()) {
        // A simple manual GPU schedule (can be further optimized).
        Var block_x("gpu_block_x"), block_y("gpu_block_y");
        Var thread_x("gpu_thread_x"), thread_y("gpu_thread_y");
        final_stage.gpu_tile(x, y, block_x, block_y, thread_x, thread_y, 16, 16, c);
        denoised.compute_root();
        ca_builder.output.compute_root();
        downscaled.compute_root();
        local_laplacian_builder.output.compute_root();

    } else {
        // High-performance manual CPU schedule implementing the Strip -> Tile architecture.
        int vec = target.template natural_vector_size<P>();
        int vec_f = target.template natural_vector_size<float>();
        const int strip_size = 32;
        const int tile_size_x = 256;

        // --- ROOT-LEVEL LOOKUP TABLES ---
        color_correct_builder.cc_matrix.compute_root();
        tone_curve_func.compute_root();

        // --- MAIN TILED EXECUTION ---
        final_stage.compute_root()
            .tile(x, y, xo, yo, xi, yi, tile_size_x, strip_size)
            .reorder(xi, yi, c, xo, yo)
            // No reorder_storage. Output will be planar: RRR...GGG...BBB...
            .parallel(yo)
            .vectorize(xi, vec);
        final_stage.bound(c, 0, 3).unroll(c);


        // --- "PER-STRIP" STAGES (Strip Waterfall) ---
        denoised.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec);

        // Schedule for Chromatic Aberration Correction.
        ca_builder.output.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec);
        ca_builder.g_interp.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec_f);
        ca_builder.block_shifts.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(ca_builder.bx, vec_f);
        ca_builder.blur_y.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(ca_builder.bx, vec_f);
        ca_builder.blur_x.compute_at(ca_builder.blur_y, ca_builder.by).vectorize(ca_builder.bx, vec_f);


        deinterleaved_hi_fi.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec);
        demosaiced.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec);
        demosaiced.bound(c, 0, 3).unroll(c);
        
        // --- Schedule for the Downscaling Stage ---
        // Create two versions of the code: one that bypasses the resize, one that does it.
        downscaled.specialize(is_no_op);
        downscaled.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec);
        downscaled.bound(c, 0, 3).unroll(c);

        // Schedule the internal producer of the resize operation.
        // It is computed per scanline of its consumer, `downscaled`.
        // We vectorize over its own pure dimension, `x_coord`.
        resize_builder.interp_y
            .compute_at(downscaled, y)
            .vectorize(resize_builder.x_coord, vec_f);


        corrected_hi_fi.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec);
        corrected_hi_fi.bound(c, 0, 3).unroll(c);
        sharpened.compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(x, vec);
        sharpened.bound(c, 0, 3).unroll(c);

#ifndef BYPASS_LAPLACIAN_PYRAMID
        local_laplacian_builder.remap_lut.compute_root();
        local_laplacian_builder.lab_f_lut.compute_root();
        local_laplacian_builder.lab_f_inv_lut.compute_root();

        // --- SCHEDULE THE PYRAMID ---
        // The high-frequency levels of the pyramid are computed per tile.
        // This includes the base of the pyramid (gPyramid[0], which is L_norm_hifi).
        local_laplacian_builder.gPyramid[0].compute_at(final_stage, xo).store_at(final_stage, yo);

        bool perform_splice = (cutover_level > 0 && cutover_level < J);

        if (perform_splice) {
            // Schedule the low-fi path intermediates that are materialized.
            // The first few stages are inlined and don't need a schedule.
            for (size_t i = 2; i < local_laplacian_builder.low_fi_intermediates.size(); ++i) {
                auto& f = local_laplacian_builder.low_fi_intermediates[i];
                f.compute_at(final_stage, yo).store_at(final_stage, yo);
            }

            // Schedule the high-frequency pyramid levels (computed per-tile)
            for (int j = 1; j < cutover_level; j++) {
                local_laplacian_builder.gPyramid[j].compute_at(final_stage, xo).store_at(final_stage, yo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec);
            }

            // Schedule the low-frequency pyramid levels (computed per-strip)
            for (int j = cutover_level; j < J; j++) {
                local_laplacian_builder.gPyramid[j].compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec);
            }
        } else {
            // If no splice, all pyramid levels are computed per tile.
            for (int j = 1; j < J; j++) {
                local_laplacian_builder.gPyramid[j].compute_at(final_stage, xo).store_at(final_stage, yo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec);
            }
        }
        
        // Schedule all pyramid reconstruction levels and their helpers.
        // The logic for whether they are strip- or tile-based must match the gPyramid schedule.
        for (int j = 0; j < J; j++) {
            if (perform_splice && j >= cutover_level) {
                 local_laplacian_builder.inLPyramid[j].compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(local_laplacian_builder.inLPyramid[j].args()[0], vec);
                 local_laplacian_builder.outLPyramid[j].compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(local_laplacian_builder.outLPyramid[j].args()[0], vec);
                 local_laplacian_builder.outGPyramid[j].compute_at(final_stage, yo).store_at(final_stage, yo).vectorize(local_laplacian_builder.outGPyramid[j].args()[0], vec);
            } else {
                 local_laplacian_builder.inLPyramid[j].compute_at(final_stage, xo).store_at(final_stage, yo).vectorize(local_laplacian_builder.inLPyramid[j].args()[0], vec);
                 local_laplacian_builder.outLPyramid[j].compute_at(final_stage, xo).store_at(final_stage, yo).vectorize(local_laplacian_builder.outLPyramid[j].args()[0], vec);
                 local_laplacian_builder.outGPyramid[j].compute_at(final_stage, xo).store_at(final_stage, yo).vectorize(local_laplacian_builder.outGPyramid[j].args()[0], vec);
            }
        }
        for (auto& f : local_laplacian_builder.high_freq_pyramid_helpers) {
            f.compute_at(final_stage, xo).store_at(final_stage, yo);
        }
        for (auto& f : local_laplacian_builder.low_freq_pyramid_helpers) {
            f.compute_at(final_stage, yo).store_at(final_stage, yo);
        }
        
#endif
        local_laplacian_builder.output.compute_at(final_stage, xo).store_at(final_stage, yo).vectorize(x, vec);
        local_laplacian_builder.output.bound(c, 0, 3).unroll(c);

        curved.compute_at(final_stage, yi).vectorize(x, vec);
        curved.bound(c, 0, 3).unroll(c);
    }
}
#endif // PIPELINE_SCHEDULE_H
