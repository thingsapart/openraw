#ifndef PIPELINE_SCHEDULE_H
#define PIPELINE_SCHEDULE_H

// This file is #included directly into the `generate` method of the CameraPipeGenerator.
// It contains the manual CPU schedule that implements the Strip -> Tile architecture.

if (this->using_autoscheduler()) {
    // Autoscheduler will determine the schedule.
} else if (this->get_target().has_gpu_feature()) {
    // A simple manual GPU schedule (can be further optimized).
    Halide::Var block_x("gpu_block_x"), block_y("gpu_block_y");
    Halide::Var thread_x("gpu_thread_x"), thread_y("gpu_thread_y");
    final_stage.gpu_tile(x, y, block_x, block_y, thread_x, thread_y, 16, 16);
    // Other stages would need GPU schedules, compute_root is a safe default for complex ones.
    denoised_raw.compute_root();
    ca_corrected.compute_root();

} else {
    // High-performance manual CPU schedule implementing the Strip -> Tile architecture.
    int vec = this->get_target().natural_vector_size(halide_proc_type);
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
    denoised_raw.compute_at(final_stage, yo).vectorize(x, vec);
    ca_corrected.compute_at(final_stage, yo).vectorize(x, vec);
    for (Halide::Func f : ca_builder.intermediates) {
        f.compute_at(final_stage, yo);
    }
    deinterleaved_hi_fi.compute_at(final_stage, yo).vectorize(x, vec);
    demosaiced.compute_at(final_stage, yo).vectorize(x, vec);
    for (Halide::Func f : demosaic_dispatcher.all_intermediates) {
        f.compute_at(final_stage, yo);
    }
    corrected_hi_fi.compute_at(final_stage, yo).vectorize(x, vec);
    sharpened.compute_at(final_stage, yo).vectorize(x, vec);

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
    local_adjustments.compute_at(final_stage, xo).vectorize(x, vec);
    curved.compute_at(final_stage, xo).vectorize(x, vec);
}

#endif // PIPELINE_SCHEDULE_H
