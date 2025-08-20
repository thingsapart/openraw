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
    Halide::Func normalized_bayer,
    CACorrectBuilder& ca_builder,
    Halide::Func deinterleaved_hi_fi,
    Halide::Func demosaiced,
    DemosaicDispatcherT<float>& demosaic_dispatcher,
    Halide::Func downscaled,
    Halide::Expr is_no_op_resize,
    ResizeBicubicBuilder& resize_builder,
    Halide::Func corrected_hi_fi,
    Halide::Func dehazed,
    Halide::Func resampled,
    Halide::Func resampled_or_bypass,
    Halide::Expr is_no_op_resample,
    Halide::Func sharpened,
    LocalLaplacianBuilder& local_laplacian_builder,
    Halide::Func curved,
    Halide::Func final_stage,
    ColorCorrectBuilder_T<P>& color_correct_builder,
    Halide::Func tone_curve_func,
    Halide::Func color_graded,
    Halide::Func srgb_to_lch,
    Halide::Func lch_to_srgb,
    Halide::Func vignette_corrected,
    Halide::Type halide_proc_type,
    Halide::Var x, Halide::Var y, Halide::Var c, Halide::Var xo, Halide::Var xi, Halide::Var yo, Halide::Var yi,
    int J, int cutover_level)
{
    using namespace Halide;

    if (is_autoscheduled) {
        // Autoscheduler will determine the schedule.
    } else if (target.has_gpu_feature()) {
        // A simple manual GPU schedule. Note the two "firebreaks" at vignette_corrected and resampled.
        Var block_x("gpu_block_x"), block_y("gpu_block_y");
        Var thread_x("gpu_thread_x"), thread_y("gpu_thread_y");

        // Phase 1 output
        vignette_corrected.compute_root().gpu_tile(x, y, block_x, block_y, thread_x, thread_y, 16, 16, c);
        // Phase 2 output
        final_stage.compute_root().gpu_tile(x, y, block_x, block_y, thread_x, thread_y, 16, 16, c);
        // Intermediate firebreak
        resampled.compute_root();
        resampled_or_bypass.specialize(is_no_op_resample);


    } else {
        // High-performance manual CPU schedule implementing a two-phase execution.
        int vec = target.template natural_vector_size<P>();
        int vec_f = target.template natural_vector_size<float>();
        const int strip_size = 32;
        const int tile_size_x = 256;

        Var byi("byi"), byo("byo");
        normalized_bayer.compute_root().split(y, byo, byi, 128).parallel(byo).vectorize(x, vec_f);

        // --- GLOBAL LOOKUP TABLES ---
        color_correct_builder.cc_matrix.compute_root();
        tone_curve_func.compute_root();

        // --- PHASE 1: Original Tiled Pipeline ---
        // This phase computes everything up to the point before resampling.
        // `vignette_corrected` is the final output of this phase, materialized at root.
        vignette_corrected.compute_root()
            .tile(x, y, xo, yo, xi, yi, tile_size_x, strip_size)
            .reorder(xi, yi, c, xo, yo)
            .parallel(yo)
            .vectorize(xi, vec_f);
        vignette_corrected.bound(c, 0, 3).unroll(c);

        // Schedule all producers relative to `vignette_corrected` instead of `final_stage`.
        denoised.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(x, vec_f);
        ca_builder.output.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(x, vec_f);
        ca_builder.g_interp.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(x, vec_f);
        ca_builder.block_shifts.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(ca_builder.bx, vec_f);
        ca_builder.blur_y.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(ca_builder.bx, vec_f);
        ca_builder.blur_x.compute_at(ca_builder.blur_y, ca_builder.by).vectorize(ca_builder.bx, vec_f);

        deinterleaved_hi_fi.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(x, vec_f);
        demosaiced.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(x, vec);
        demosaiced.bound(c, 0, 3).unroll(c);

        downscaled.specialize(is_no_op_resize);
        downscaled.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(x, vec);
        downscaled.bound(c, 0, 3).unroll(c);

        resize_builder.interp_y.compute_at(downscaled, y).vectorize(resize_builder.x_coord, vec_f);

        corrected_hi_fi.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(x, vec);
        corrected_hi_fi.bound(c, 0, 3).unroll(c);

#ifndef BYPASS_LAPLACIAN_PYRAMID
        local_laplacian_builder.remap_lut.compute_root();

        local_laplacian_builder.gPyramid[0].compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo);
        bool perform_splice = (cutover_level > 0 && cutover_level < J);

        if (perform_splice) {
            for (auto& f : local_laplacian_builder.low_fi_intermediates) f.compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo);
            if (local_laplacian_builder.lowfi_resize_builder) {
                auto& builder = local_laplacian_builder.lowfi_resize_builder;
                Var hy = builder->output.args()[1];
                builder->interp_y.compute_at(builder->output, hy).vectorize(builder->x_coord, vec_f);
            }
            for (int j = 1; j < cutover_level; j++) local_laplacian_builder.gPyramid[j].compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec_f);
            for (int j = cutover_level; j < J; j++) local_laplacian_builder.gPyramid[j].compute_at(vignette_corrected, yo).store_at(vignette_corrected, yo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec_f);
        } else {
            for (int j = 1; j < J; j++) local_laplacian_builder.gPyramid[j].compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec_f);
        }

        for (int j = 0; j < J; j++) {
            auto& helpers = (perform_splice && j >= cutover_level) ? local_laplacian_builder.low_freq_pyramid_helpers : local_laplacian_builder.high_freq_pyramid_helpers;
            auto compute_loc = (perform_splice && j >= cutover_level) ? yo : xo;
            for (auto& f : helpers) f.compute_at(vignette_corrected, compute_loc).store_at(vignette_corrected, yo);
            local_laplacian_builder.inLPyramid[j].compute_at(vignette_corrected, compute_loc).store_at(vignette_corrected, yo).vectorize(local_laplacian_builder.inLPyramid[j].args()[0], vec_f);
            local_laplacian_builder.outLPyramid[j].compute_at(vignette_corrected, compute_loc).store_at(vignette_corrected, yo).vectorize(local_laplacian_builder.outLPyramid[j].args()[0], vec_f);
        }
        for (int b = 0; b < J; ++b) {
            for (int j = 0; j <= b; ++j) {
                auto& f = local_laplacian_builder.reconstructedGPyramid[b][j];
                auto compute_loc = (perform_splice && j >= cutover_level) ? yo : xo;
                f.compute_at(vignette_corrected, compute_loc).store_at(vignette_corrected, yo).vectorize(f.args()[0], vec_f);
            }
        }
#endif
        // These are all pointwise stages that lead into vignette_corrected.
        dehazed.compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo).vectorize(x, vec_f).bound(c,0,3).unroll(c);
        srgb_to_lch.compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo).vectorize(x, vec_f).bound(c,0,3).unroll(c);
        local_laplacian_builder.output.compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo).vectorize(x, vec_f).bound(c,0,3).unroll(c);
        color_graded.compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo).vectorize(x, vec_f).bound(c,0,3).unroll(c);
        lch_to_srgb.compute_at(vignette_corrected, xo).store_at(vignette_corrected, yo).vectorize(x, vec_f).bound(c,0,3).unroll(c);


        // --- PHASE 2: Geometry, Sharpen, and Final Conversion ---

        // This is the geometry "firebreak". It consumes the entire `vignette_corrected` buffer.
        resampled.compute_root()
            .parallel(y)
            .vectorize(x, vec_f);
        resampled.bound(c, 0, 3).unroll(c);

        // This is the final output phase. It consumes the `resampled_or_bypass` buffer.
        final_stage.compute_root()
            .tile(x, y, xo, yo, xi, yi, tile_size_x, strip_size)
            .reorder(xi, yi, c, xo, yo)
            .parallel(yo)
            .vectorize(xi, vec);
        final_stage.bound(c, 0, 3).unroll(c);

        // Give the bypass switch a concrete schedule so we can specialize it.
        // It's pointwise, so compute it at the same tile level as its consumer, `sharpened`.
        resampled_or_bypass
            .compute_at(final_stage, xo).store_at(final_stage, yo)
            .vectorize(x, vec_f)
            .bound(c, 0, 3).unroll(c);
        resampled_or_bypass.specialize(is_no_op_resample);

        // Schedule the final pointwise stages relative to `final_stage`.
        sharpened.compute_at(final_stage, xo).store_at(final_stage, yo).vectorize(x, vec).bound(c, 0, 3).unroll(c);
        curved.compute_at(final_stage, yi).vectorize(x, vec).bound(c, 0, 3).unroll(c);
    }
}
#endif // PIPELINE_SCHEDULE_H

