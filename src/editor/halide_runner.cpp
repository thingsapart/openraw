#include "halide_runner.h"
#include "tone_curve_utils.h"

#include <algorithm>
#include <iostream>

// This project only builds the f32 pipeline for the UI for simplicity.
#include "camera_pipe_f32_lib.h"

static void ConvertPlanarToInterleaved(const Halide::Runtime::Buffer<uint8_t>& planar_in, std::vector<uint8_t>& interleaved_out) {
    if (planar_in.data() == nullptr) {
        interleaved_out.clear();
        return;
    }
    const int width = planar_in.width();
    const int height = planar_in.height();
    const int channels = planar_in.channels();
    const size_t total_pixels = width * height;

    interleaved_out.resize(total_pixels * channels);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            for (int c = 0; c < channels; c++) {
                interleaved_out[(y * width + x) * channels + c] = planar_in(x, y, c);
            }
        }
    }
}

static void run_pipeline_instance(AppState& state, float downscale_factor, Halide::Runtime::Buffer<uint8_t>& output_buffer, const std::string& view_name) {
    if (state.input_image.data() == nullptr) {
        return;
    }

    const int raw_w = state.input_image.width() - 32;
    const int raw_h = state.input_image.height() - 24;
    int out_width = static_cast<int>(raw_w / downscale_factor);
    int out_height = static_cast<int>(raw_h / downscale_factor);
    
    printf("\n--- Preparing Halide run for: %s ---\n", view_name.c_str());
    printf("  Downscale Factor: %.4f, Calculated Output: %d x %d\n", downscale_factor, out_width, out_height);
    
    // This check is now a fallback; the factor clamping should prevent this.
    if (out_width < 32 || out_height < 32) {
        printf("  SKIPPING: Output size is too small.\n");
        return;
    }

    if (output_buffer.data() == nullptr || output_buffer.width() != out_width || output_buffer.height() != out_height) {
        output_buffer = Halide::Runtime::Buffer<uint8_t>(out_width, out_height, 3);
    }
    
    ToneCurveUtils tone_curve_util(state.params);
    Halide::Runtime::Buffer<uint16_t, 2> tone_curve_lut = tone_curve_util.get_lut_for_halide();

    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f},
                               {-0.3576f, 1.0615f, 1.5949f, -37.1158f},
                               {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};
    float _matrix_7000[][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f},
                               {-0.3826f, 1.5906f, -0.2080f, -25.4311f},
                               {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};
    Halide::Runtime::Buffer<float, 2> matrix_3200(4, 3), matrix_7000(4, 3);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) {
        matrix_3200(j, i) = _matrix_3200[i][j];
        matrix_7000(j, i) = _matrix_7000[i][j];
    }

    int blackLevel = 25;
    int whiteLevel = 1023;
    float denoise_strength_norm = std::max(0.0f, std::min(1.0f, state.params.denoise_strength / 100.0f));
    int demosaic_id = 3;
    if (state.params.demosaic_algorithm == "ahd") demosaic_id = 0;
    else if (state.params.demosaic_algorithm == "lmmse") demosaic_id = 1;
    else if (state.params.demosaic_algorithm == "ri") demosaic_id = 2;

    int result = camera_pipe_f32(state.input_image, downscale_factor, demosaic_id, matrix_3200, matrix_7000,
                                 state.params.color_temp, state.params.tint, state.params.ca_strength,
                                 denoise_strength_norm, state.params.denoise_eps,
                                 blackLevel, whiteLevel, tone_curve_lut,
                                 0.f, 0.f, 0.f,
                                 state.params.ll_detail, state.params.ll_clarity, state.params.ll_shadows, 
                                 state.params.ll_highlights, state.params.ll_blacks, state.params.ll_whites,
                                 output_buffer);

    if (result != 0) {
        std::cerr << "Halide pipeline returned an error: " << result << std::endl;
    }
}

void RunHalidePipelines(AppState& state) {
    if (state.input_image.data() == nullptr || state.main_view_size.x <= 0 || state.thumb_view_size.x <= 0) {
        return;
    }
    
    const float source_w = state.input_image.width() - 32;

    // --- Main View Pipeline ---
    float fit_scale = std::min(state.main_view_size.x / source_w, state.main_view_size.y / (state.input_image.height() - 24));
    float on_screen_width = source_w * fit_scale * state.zoom;
    float downscale_factor_main = source_w / on_screen_width;

    // Calculate the maximum safe downscale factor to avoid crashing the pipeline
    const float min_pipeline_width = 256.0f;
    float max_downscale_factor = source_w / min_pipeline_width;
    // Use the smaller of the two: either what the user requested, or the max safe factor
    downscale_factor_main = std::min(downscale_factor_main, max_downscale_factor);
    // Always render at least 1:1, let the UI handle up-scaling
    downscale_factor_main = std::max(1.0f, downscale_factor_main);

    run_pipeline_instance(state, downscale_factor_main, state.main_output_planar, "Main View");
    ConvertPlanarToInterleaved(state.main_output_planar, state.main_output_interleaved);

    // --- Thumbnail Pipeline ---
    float downscale_factor_thumb = source_w / state.thumb_view_size.x;
    downscale_factor_thumb = std::max(1.0f, downscale_factor_thumb);
    run_pipeline_instance(state, downscale_factor_thumb, state.thumb_output_planar, "Thumbnail");
    ConvertPlanarToInterleaved(state.thumb_output_planar, state.thumb_output_interleaved);
}
