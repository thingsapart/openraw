#ifndef PIPELINE_SCHEDULE_H
#define PIPELINE_SCHEDULE_H

#include "Halide.h"
#include "stage_ca_correct.h"
#include "stage_demosaic.h"
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
    Halide::Func corrected_hi_fi,
    Halide::Func sharpened,
    LocalLaplacianBuilder& local_laplacian_builder,
    Halide::Func curved,
    Halide::Func final_stage,
    ColorCorrectBuilder_T<P>& color_correct_builder,
    Halide::Func tone_curve_func,
    Halide::Type halide_proc_type,
    Halide::Var x, Halide::Var y, Halide::Var c, Halide::Var xo, Halide::Var xi, Halide::Var yo, Halide::Var yi)
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
        local_laplacian_builder.output.compute_root();

    } else {
        // High-performance manual CPU schedule implementing the Strip -> Tile architecture.
        int vec = target.template natural_vector_size<P>();
        const int strip_size = 32;
        const int tile_size_x = 256;

        // --- ROOT-LEVEL LOOKUP TABLES ---
        color_correct_builder.cc_matrix.compute_root();
        tone_curve_func.compute_root();

        // --- MAIN TILED EXECUTION ---
        final_stage.compute_root()
            .split(y, yo, yi, strip_size)
            .split(x, xo, xi, tile_size_x)
            .reorder(xi, yi, c, xo, yo)
            .parallel(yo)
            .vectorize(xi, vec);

        // --- "PER-STRIP" STAGES (Strip Waterfall) ---
        denoised.compute_at(final_stage, yo).vectorize(x, vec);
        
        ca_builder.output.compute_at(final_stage, yo).vectorize(x, vec);
        for (Func f : ca_builder.intermediates) {
            f.compute_at(final_stage, yo);
        }

        deinterleaved_hi_fi.compute_at(final_stage, yo).vectorize(x, vec);
        demosaiced.compute_at(final_stage, yo).vectorize(x, vec);
        for (Func f : demosaic_dispatcher.all_intermediates) {
            f.compute_at(final_stage, yo);
        }
        corrected_hi_fi.compute_at(final_stage, yo).vectorize(x, vec);
        sharpened.compute_at(final_stage, yo).vectorize(x, vec);

#ifndef BYPASS_LAPLACIAN_PYRAMID
        local_laplacian_builder.remap_lut.compute_root();
        // --- "PER-TILE" STAGES (Forked Pyramid and Reconstruction) ---
        const int J = local_laplacian_builder.pyramid_levels;
        const int cutover = 4;

        // LOW-FI path is computed per-strip.
        for (auto& f : local_laplacian_builder.low_fi_intermediates) {
            f.compute_at(final_stage, yo).vectorize(f.args()[0], vec);
        }
        // LOW-FREQUENCY pyramid levels and their helpers are computed per-strip.
        for (int j = cutover; j < J; j++) {
            local_laplacian_builder.gPyramid[j].compute_at(final_stage, yo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec);
            local_laplacian_builder.inLPyramid[j].compute_at(final_stage, yo).vectorize(local_laplacian_builder.inLPyramid[j].args()[0], vec);
            local_laplacian_builder.outLPyramid[j].compute_at(final_stage, yo).vectorize(local_laplacian_builder.outLPyramid[j].args()[0], vec);
            local_laplacian_builder.outGPyramid[j].compute_at(final_stage, yo).vectorize(local_laplacian_builder.outGPyramid[j].args()[0], vec);
        }
        for (auto& f : local_laplacian_builder.low_freq_pyramid_helpers) {
            f.compute_at(final_stage, yo);
        }

        // HIGH-FREQUENCY pyramid levels and their helpers are computed per-tile.
        for (int j = 0; j < cutover; j++) {
            local_laplacian_builder.gPyramid[j].compute_at(final_stage, xo).vectorize(local_laplacian_builder.gPyramid[j].args()[0], vec);
            local_laplacian_builder.inLPyramid[j].compute_at(final_stage, xo).vectorize(local_laplacian_builder.inLPyramid[j].args()[0], vec);
            local_laplacian_builder.outLPyramid[j].compute_at(final_stage, xo).vectorize(local_laplacian_builder.outLPyramid[j].args()[0], vec);
            local_laplacian_builder.outGPyramid[j].compute_at(final_stage, xo).vectorize(local_laplacian_builder.outGPyramid[j].args()[0], vec);
        }
        for (auto& f : local_laplacian_builder.high_freq_pyramid_helpers) {
            f.compute_at(final_stage, xo);
        }
        
        // Fused, pointwise stages are scheduled per-tile.
        for (auto& f : local_laplacian_builder.high_fi_intermediates) {
            f.compute_at(final_stage, xo);
        }
        for (auto& f : local_laplacian_builder.reconstruction_intermediates) {
            f.compute_at(final_stage, xo).vectorize(x, vec);
        }
#else
        // Schedule for the BYPASS debug path
        for (auto& f : local_laplacian_builder.high_fi_intermediates) {
            f.compute_at(final_stage, xo);
        }
#endif
        local_laplacian_builder.output.compute_at(final_stage, xo).vectorize(x, vec);
        curved.compute_at(final_stage, xo).vectorize(x, vec);
    }
}
#endif // PIPELINE_SCHEDULE_H
