#include "halide_runner.h"
#include "tone_curve_utils.h"
#include "color_tools.h"

#include <algorithm>
#include <iostream>
#include <numeric> // for std::accumulate
#include <cstring> // For memcpy
#include <cmath>   // For powf

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

// New function to compute histograms from the 8-bit preview image.
static void ComputeHistograms(AppState& state) {
    const auto& preview_image = state.thumb_output_planar;
    if (preview_image.data() == nullptr) {
        return;
    }

    const int width = preview_image.width();
    const int height = preview_image.height();
    const int bins = 256;

    std::vector<int> counts_r(bins, 0), counts_g(bins, 0), counts_b(bins, 0), counts_luma(bins, 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // The preview_image from the pipeline is already gamma-corrected
            // via the tone curve. We can bin the values directly.
            uint8_t r = preview_image(x, y, 0);
            uint8_t g = preview_image(x, y, 1);
            uint8_t b = preview_image(x, y, 2);

            // Calculate luma from the gamma-corrected RGB values.
            uint8_t luma = static_cast<uint8_t>(0.299f * r + 0.587f * g + 0.114f * b);

            counts_r[r]++;
            counts_g[g]++;
            counts_b[b]++;
            counts_luma[luma]++;
        }
    }

    // Find the max count to normalize the histograms for plotting.
    // To prevent clipped blacks (bin 0) and whites (bin 255) from
    // "smashing" the rest of the histogram, we find the max value
    // only in the range [1, 254].
    int max_count = 0;
    auto find_max = [&](const std::vector<int>& counts) {
        if (counts.size() == bins) {
             max_count = std::max(max_count, *std::max_element(counts.begin() + 1, counts.end() - 1));
        }
    };
    find_max(counts_r);
    find_max(counts_g);
    find_max(counts_b);
    find_max(counts_luma);

    if (max_count == 0) max_count = 1; // Avoid division by zero

    // Resize and fill the float vectors in AppState with normalized values
    auto normalize_and_store = [&](std::vector<float>& dest, const std::vector<int>& counts) {
        dest.resize(bins);
        for(int i = 0; i < bins; ++i) {
            float normalized_val = static_cast<float>(counts[i]) / max_count;
            // Clamp the value to 1.0f. This handles the outlier bins (0 and 255)
            // so they don't "shoot out" of the top of the histogram view.
            dest[i] = std::min(normalized_val, 1.0f);
        }
    };

    normalize_and_store(state.histogram_r, counts_r);
    normalize_and_store(state.histogram_g, counts_g);
    normalize_and_store(state.histogram_b, counts_b);
    normalize_and_store(state.histogram_luma, counts_luma);
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

    if (out_width < 32 || out_height < 32) {
        printf("  SKIPPING: Output size is too small.\n");
        return;
    }

    if (output_buffer.data() == nullptr || output_buffer.width() != out_width || output_buffer.height() != out_height) {
        output_buffer = Halide::Runtime::Buffer<uint8_t>(out_width, out_height, 3);
    }

    Halide::Runtime::Buffer<uint16_t, 2> pipeline_lut = ToneCurveUtils::generate_pipeline_lut(state.params);
    Halide::Runtime::Buffer<float, 4> color_grading_lut = HostColor::generate_color_lut(state.params);

    if (view_name == "Thumbnail") {
        memcpy(state.pipeline_tone_curve_lut.data(), pipeline_lut.data(), pipeline_lut.size_in_bytes());
        ToneCurveUtils::generate_linear_lut(state.params, state.ui_tone_curve_lut);
    }


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
    float exposure_multiplier = powf(2.0f, state.params.exposure);
    int demosaic_id = 3;
    if (state.params.demosaic_algorithm == "ahd") demosaic_id = 0;
    else if (state.params.demosaic_algorithm == "lmmse") demosaic_id = 1;
    else if (state.params.demosaic_algorithm == "ri") demosaic_id = 2;

    int result = camera_pipe_f32(state.input_image, downscale_factor, demosaic_id, matrix_3200, matrix_7000,
                                 state.params.color_temp, state.params.tint, exposure_multiplier, state.params.ca_strength,
                                 denoise_strength_norm, state.params.denoise_eps,
                                 blackLevel, whiteLevel, pipeline_lut,
                                 0.f, 0.f, 0.f,
                                 state.params.ll_detail, state.params.ll_clarity, state.params.ll_shadows,
                                 state.params.ll_highlights, state.params.ll_blacks, state.params.ll_whites,
                                 color_grading_lut,
                                 state.params.vignette_amount, state.params.vignette_midpoint, state.params.vignette_roundness, state.params.vignette_highlights,
                                 state.params.dehaze_strength,
                                 output_buffer);

    if (result != 0) {
        std::cerr << "Halide pipeline returned an error: " << result << std::endl;
    }
}

void RunHalidePipelines(AppState& state) {
    if (state.input_image.data() == nullptr || state.main_view_size.x <= 0) {
        return;
    }

    const float raw_w = state.input_image.width() - 32;
    const float raw_h = state.input_image.height() - 24;

    // --- Main View Pipeline ---
    float fit_scale = std::min(state.main_view_size.x / raw_w, state.main_view_size.y / raw_h);
    float on_screen_width = raw_w * fit_scale * state.zoom;
    float downscale_factor_main = raw_w / on_screen_width;

    const float min_pipeline_width = 256.0f;
    float max_downscale_factor = raw_w / min_pipeline_width;
    downscale_factor_main = std::min(downscale_factor_main, max_downscale_factor);
    downscale_factor_main = std::max(1.0f, downscale_factor_main);

    run_pipeline_instance(state, downscale_factor_main, state.main_output_planar, "Main View");
    ConvertPlanarToInterleaved(state.main_output_planar, state.main_output_interleaved);

    // --- Thumbnail & Histogram Pipeline ---
    const float PREVIEW_LONGEST_DIM = 1024.0f;
    float downscale_factor_thumb = (raw_w > raw_h) ? (raw_w / PREVIEW_LONGEST_DIM) : (raw_h / PREVIEW_LONGEST_DIM);
    downscale_factor_thumb = std::max(1.0f, downscale_factor_thumb);

    run_pipeline_instance(state, downscale_factor_thumb, state.thumb_output_planar, "Thumbnail");

    ComputeHistograms(state);
    ConvertPlanarToInterleaved(state.thumb_output_planar, state.thumb_output_interleaved);
}

