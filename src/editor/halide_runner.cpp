#include "editor/halide_runner.h"
#include <cmath>
#include <iostream>
#include <algorithm>

// The editor exclusively uses the float32 pipeline.
// Define this macro to ensure the correct generated header is included.
#define PIPELINE_PRECISION_F32

// To enable Lensfun support, compile with -DUSE_LENSFUN and link against liblensfun.
#ifdef USE_LENSFUN
#include "lensfun/lensfun.h"
#endif

// Conditionally include the generated pipeline headers
#if defined(PIPELINE_PRECISION_F32)
#include "camera_pipe_f32_lib.h"
#elif defined(PIPELINE_PRECISION_U16)
#include "camera_pipe_u16_lib.h"
#else
#error "PIPELINE_PRECISION_F32 or PIPELINE_PRECISION_U16 must be defined"
#endif

namespace { // Anonymous namespace for local helpers

// --- Lens Correction LUT Generation (Copied from process.cpp) ---
namespace LensCorrection {
    const int LUT_SIZE = 2048;
    const float MAX_RD_SQUARED_NORM = 3.0f;

    float solve_cubic_poly3(float p, float q) {
        float p_3 = p / 3.0f;
        float q_2 = q / 2.0f;
        float discriminant = q_2 * q_2 + p_3 * p_3 * p_3;

        if (discriminant >= 0) {
            float root_discriminant = sqrtf(discriminant);
            float term1 = cbrtf(-q_2 + root_discriminant);
            float term2 = cbrtf(-q_2 - root_discriminant);
            return term1 + term2;
        } else {
            float r = sqrtf(-p_3 * -p_3 * -p_3);
            float phi = acosf(-q_2 / r);
            float two_sqrt_p3 = 2.0f * sqrtf(-p_3);
            return two_sqrt_p3 * cosf(phi / 3.0f);
        }
    }

    float solve_poly5(float k1, float k2, float rd) {
        float ru = rd; // Initial guess
        for (int i = 0; i < 4; ++i) {
            float ru2 = ru * ru;
            float ru3 = ru2 * ru;
            float ru4 = ru2 * ru2;
            float ru5 = ru4 * ru;
            float f = k2 * ru5 + k1 * ru3 + ru - rd;
            float f_prime = 5.0f * k2 * ru4 + 3.0f * k1 * ru2 + 1.0f;
            if (fabsf(f_prime) < 1e-6) break;
            ru -= f / f_prime;
        }
        return ru;
    }

    float solve_ptlens(float a, float b, float c, float rd) {
        float ru = rd; // Initial guess
        float d = 1.0f - a - b - c;
        for (int i = 0; i < 4; ++i) {
            float ru2 = ru * ru;
            float ru3 = ru2 * ru;
            float ru4 = ru2 * ru2;
            float f = a * ru4 + b * ru3 + c * ru2 + d * ru - rd;
            float f_prime = 4.0f * a * ru3 + 3.0f * b * ru2 + 2.0f * c * ru + d;
            if (fabsf(f_prime) < 1e-6) break;
            ru -= f / f_prime;
        }
        return ru;
    }

    Halide::Runtime::Buffer<float, 1> generate_identity_lut() {
        Halide::Runtime::Buffer<float, 1> lut(LUT_SIZE);
        for (int i = 0; i < LUT_SIZE; ++i) {
            lut(i) = 1.0f;
        }
        return lut;
    }

#ifdef USE_LENSFUN
    Halide::Runtime::Buffer<float, 1> generate_distortion_lut(const lfLensCalibDistortion& model) {
        Halide::Runtime::Buffer<float, 1> lut(LUT_SIZE);
        for (int i = 0; i < LUT_SIZE; ++i) {
            float rd_sq_norm = (float)i * MAX_RD_SQUARED_NORM / (float)(LUT_SIZE - 1);
            float rd_norm = sqrtf(rd_sq_norm);
            float ru_norm = rd_norm;

            switch (model.Model) {
                case LF_DIST_MODEL_POLY3: {
                    float k1 = model.Terms[0];
                    if (fabsf(k1) > 1e-6f) {
                        ru_norm = solve_cubic_poly3((1.0f - k1) / k1, -rd_norm / k1);
                    }
                    break;
                }
                case LF_DIST_MODEL_POLY5: {
                    ru_norm = solve_poly5(model.Terms[0], model.Terms[1], rd_norm);
                    break;
                }
                case LF_DIST_MODEL_PTLENS: {
                    ru_norm = solve_ptlens(model.Terms[0], model.Terms[1], model.Terms[2], rd_norm);
                    break;
                }
                default: break;
            }
            lut(i) = (rd_norm > 1e-5f) ? (ru_norm / rd_norm) : 1.0f;
        }
        return lut;
    }
#endif
} // namespace LensCorrection
} // namespace


void RunHalidePipelines(AppState& state) {
    const ProcessConfig& cfg = state.params;

    // --- Common Setup ---
    int demosaic_id = 3; // default to 'fast'
    if (cfg.demosaic_algorithm == "ahd") demosaic_id = 0;
    else if (cfg.demosaic_algorithm == "lmmse") demosaic_id = 1;
    else if (cfg.demosaic_algorithm == "ri") demosaic_id = 2;

    float exposure_multiplier = powf(2.0f, cfg.exposure);
    float denoise_strength_norm = std::max(0.0f, std::min(1.0f, cfg.denoise_strength / 100.0f));

    // Generate the tone curve LUT and store it in the state to be passed to the pipeline.
    state.pipeline_tone_curve_lut = ToneCurveUtils::generate_pipeline_lut(cfg);
    auto color_grading_lut = HostColor::generate_color_lut(cfg);
    auto distortion_lut = LensCorrection::generate_identity_lut(); // Start with identity

#ifdef USE_LENSFUN
    bool needs_lensfun = !cfg.camera_make.empty() && !cfg.camera_model.empty() &&
                         cfg.lens_profile_name != "None" && !cfg.lens_profile_name.empty();
    bool lensfun_applied = false;
    if (needs_lensfun && state.lensfun_db) {
        const lfCamera** cams = lf_db_find_cameras(state.lensfun_db.get(), cfg.camera_make.c_str(), cfg.camera_model.c_str());
        if (cams && cams[0]) {
            const lfCamera* cam = cams[0];
            const lfLens** lenses = lf_db_find_lenses_hd(state.lensfun_db.get(), cam, nullptr, cfg.lens_profile_name.c_str(), 0);
            if (lenses && lenses[0]) {
                const lfLens* lens = lenses[0];
                lfLensCalibDistortion dist_model;
                if (lens->InterpolateDistortion(cfg.focal_length, dist_model)) {
                    if (dist_model.Model == LF_DIST_MODEL_POLY3 || dist_model.Model == LF_DIST_MODEL_POLY5 || dist_model.Model == LF_DIST_MODEL_PTLENS) {
                        distortion_lut = LensCorrection::generate_distortion_lut(dist_model);
                        lensfun_applied = true;
                    }
                }
            }
            if (lenses) lf_free(lenses);
        }
        if (cams) lf_free(cams);
    }

    // If a lensfun profile was NOT applied, check for manual overrides.
    if (!lensfun_applied) {
        const float e = 1e-6f;
        if (fabsf(cfg.dist_k1) > e || fabsf(cfg.dist_k2) > e || fabsf(cfg.dist_k3) > e) {
            lfLensCalibDistortion manual_model;
            manual_model.Model = LF_DIST_MODEL_POLY5; // Treat manual k1/k2 as POLY5
            manual_model.Terms[0] = cfg.dist_k1;
            manual_model.Terms[1] = cfg.dist_k2;
            // Note: k3 is ignored as POLY5 solver only uses k1, k2.
            distortion_lut = LensCorrection::generate_distortion_lut(manual_model);
        }
    }
#endif

    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f},
                               {-0.3576f, 1.0615f, 1.5949f, -37.1158f},
                               {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};
    float _matrix_7000[][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f},
                               {-0.3826f, 1.5906f, -0.2080f, -25.4311f},
                               {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};
    Halide::Runtime::Buffer<float, 2> matrix_3200(4, 3), matrix_7000(4, 3);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            matrix_3200(j, i) = _matrix_3200[i][j];
            matrix_7000(j, i) = _matrix_7000[i][j];
        }
    }

    // --- Main Preview Pipeline ---
    {
        float downscale_factor = powf(2.0f, state.preview_downsample);
        int out_width = static_cast<int>((state.input_image.width() - 32) / downscale_factor);
        int out_height = static_cast<int>((state.input_image.height() - 24) / downscale_factor);

        if (!state.main_output_planar.data() || state.main_output_planar.width() != out_width || state.main_output_planar.height() != out_height) {
            state.main_output_planar = Halide::Runtime::Buffer<uint8_t>(std::vector<int>{out_width, out_height, 3});
        }

        int result = camera_pipe_f32(state.input_image, downscale_factor, demosaic_id, matrix_3200, matrix_7000,
                                  cfg.color_temp, cfg.tint, exposure_multiplier, cfg.ca_strength,
                                  denoise_strength_norm, cfg.denoise_eps,
                                  state.blackLevel, state.whiteLevel, state.pipeline_tone_curve_lut,
                                  0.f, 0.f, 0.f, /* sharpen */
                                  cfg.ll_detail, cfg.ll_clarity, cfg.ll_shadows, cfg.ll_highlights, cfg.ll_blacks, cfg.ll_whites,
                                  color_grading_lut,
                                  cfg.vignette_amount, cfg.vignette_midpoint, cfg.vignette_roundness, cfg.vignette_highlights,
                                  cfg.dehaze_strength,
                                  distortion_lut,
                                  cfg.ca_red_cyan, cfg.ca_blue_yellow,
                                  cfg.geo_rotate, cfg.geo_scale, cfg.geo_aspect,
                                  cfg.geo_keystone_v, cfg.geo_keystone_h,
                                  cfg.geo_offset_x, cfg.geo_offset_y,
                                  state.main_output_planar);

        if (result != 0) { std::cerr << "Main Halide pipeline returned an error: " << result << std::endl; }

        state.main_output_interleaved.resize(out_width * out_height * 3);
        // The pipeline's output is planar (x, y, c). We interleave it for OpenGL.
        for (int y = 0; y < out_height; ++y) {
            for (int x = 0; x < out_width; ++x) {
                int inter_idx = (y * out_width + x) * 3;
                state.main_output_interleaved[inter_idx + 0] = state.main_output_planar(x, y, 0);
                state.main_output_interleaved[inter_idx + 1] = state.main_output_planar(x, y, 1);
                state.main_output_interleaved[inter_idx + 2] = state.main_output_planar(x, y, 2);
            }
        }
    }

    // --- Thumbnail Pipeline (for histogram/preview) ---
    {
        const int thumb_width = 256;
        float thumb_downscale = static_cast<float>(state.input_image.width() - 32) / thumb_width;
        int thumb_height = static_cast<int>((state.input_image.height() - 24) / thumb_downscale);

        if (!state.thumb_output_planar.data() || state.thumb_output_planar.width() != thumb_width || state.thumb_output_planar.height() != thumb_height) {
            state.thumb_output_planar = Halide::Runtime::Buffer<uint8_t>(std::vector<int>{thumb_width, thumb_height, 3});
        }

        int result = camera_pipe_f32(state.input_image, thumb_downscale, demosaic_id, matrix_3200, matrix_7000,
                                  cfg.color_temp, cfg.tint, exposure_multiplier, cfg.ca_strength,
                                  denoise_strength_norm, cfg.denoise_eps,
                                  state.blackLevel, state.whiteLevel, state.pipeline_tone_curve_lut,
                                  0.f, 0.f, 0.f, /* sharpen */
                                  cfg.ll_detail, cfg.ll_clarity, cfg.ll_shadows, cfg.ll_highlights, cfg.ll_blacks, cfg.ll_whites,
                                  color_grading_lut,
                                  cfg.vignette_amount, cfg.vignette_midpoint, cfg.vignette_roundness, cfg.vignette_highlights,
                                  cfg.dehaze_strength,
                                  distortion_lut,
                                  cfg.ca_red_cyan, cfg.ca_blue_yellow,
                                  cfg.geo_rotate, cfg.geo_scale, cfg.geo_aspect,
                                  cfg.geo_keystone_v, cfg.geo_keystone_h,
                                  cfg.geo_offset_x, cfg.geo_offset_y,
                                  state.thumb_output_planar);

        if (result != 0) { std::cerr << "Thumbnail Halide pipeline returned an error: " << result << std::endl; }

        // Interleave the thumbnail for OpenGL texture update.
        state.thumb_output_interleaved.resize(thumb_width * thumb_height * 3);
        for (int y = 0; y < thumb_height; ++y) {
            for (int x = 0; x < thumb_width; ++x) {
                int inter_idx = (y * thumb_width + x) * 3;
                state.thumb_output_interleaved[inter_idx + 0] = state.thumb_output_planar(x, y, 0);
                state.thumb_output_interleaved[inter_idx + 1] = state.thumb_output_planar(x, y, 1);
                state.thumb_output_interleaved[inter_idx + 2] = state.thumb_output_planar(x, y, 2);
            }
        }

        // Calculate histograms from the processed thumbnail
        const int hist_size = 256;
        state.histogram_r.assign(hist_size, 0);
        state.histogram_g.assign(hist_size, 0);
        state.histogram_b.assign(hist_size, 0);
        state.histogram_luma.assign(hist_size, 0);

        for (int y = 0; y < thumb_height; ++y) {
            for (int x = 0; x < thumb_width; ++x) {
                uint8_t r = state.thumb_output_planar(x, y, 0);
                uint8_t g = state.thumb_output_planar(x, y, 1);
                uint8_t b = state.thumb_output_planar(x, y, 2);
                uint8_t luma = static_cast<uint8_t>(0.299f * r + 0.587f * g + 0.114f * b);
                state.histogram_r[r]++;
                state.histogram_g[g]++;
                state.histogram_b[b]++;
                state.histogram_luma[luma]++;
            }
        }

        // Normalize histograms
        auto normalize_hist = [](std::vector<float>& hist) {
            float max_val = 0;
            for (float v : hist) max_val = std::max(max_val, v);
            if (max_val > 0) {
                for (float& v : hist) v /= max_val;
            }
        };
        normalize_hist(state.histogram_r);
        normalize_hist(state.histogram_g);
        normalize_hist(state.histogram_b);
        normalize_hist(state.histogram_luma);
    }
}
