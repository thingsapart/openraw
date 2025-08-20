#include <cstdint>
#include <string>
#include <map>
#include <stdexcept>
#include <iostream>
#include <cstdlib> // For getenv
#include <chrono>  // For timing
#include <limits>  // For numeric_limits
#include <cmath>   // for powf, cbrtf, sqrtf
#include <iomanip> // for std::setprecision
#include <memory>  // for std::shared_ptr
#include <tuple>   // for std::tuple
#include <vector>  // for std::vector

// To enable Lensfun support, compile with -DUSE_LENSFUN and link against liblensfun.
#ifdef USE_LENSFUN
#include "lensfun/lensfun.h"
#endif

#include "raw_load.h"
#include "HalideBuffer.h"
#include "halide_image_io.h"
#include "halide_malloc_trace.h"
#include "tone_curve_utils.h"
#include "process_options.h"
#include "color_tools.h"
#include "simple_timer.h"
#include "pipeline_utils.h" // Use the new shared utility header

// Conditionally include the generated pipeline headers based on the
// macro defined by CMake.
#if defined(PIPELINE_PRECISION_F32)
#include "camera_pipe_f32_lib.h"
#elif defined(PIPELINE_PRECISION_U16)
#include "camera_pipe_u16_lib.h"
#else
#error "PIPELINE_PRECISION_F32 or PIPELINE_PRECISION_U16 must be defined"
#endif

// By default, the auto-scheduled pipeline is not compiled or run.
#define NO_AUTO_SCHEDULE
#ifndef NO_AUTO_SCHEDULE
    #if defined(PIPELINE_PRECISION_F32)
    #include "camera_pipe_f32_auto_schedule_lib.h"
    #elif defined(PIPELINE_PRECISION_U16)
    #include "camera_pipe_u16_auto_schedule_lib.h"
    #endif
#endif

using namespace Halide::Runtime;
using namespace Halide::Tools;

namespace { // Anonymous namespace for local helpers

// --- DEBUG HELPER ---
void print_lut_sample(const Buffer<float, 4>& lut) {
    const int size = lut.dim(0).extent();
    const int mid = size / 2;
    const int end = size - 1;

    printf("\n--- 3D LUT Sample (L*, C*, h*) ---\n");
    printf("        Input Lch             |         Output L'c'h'\n");
    printf("------------------------------|----------------------------------\n");

    auto print_row = [&](int l, int c, int h, const std::string& label) {
        float l_in = (float)l/(size-1)*100;
        float c_in = (float)c/(size-1)*150;
        float h_in = ((float)h/(size-1)*360)-180;

        float l_out = lut(l, c, h, 0);
        float c_out = lut(l, c, h, 1);
        float h_out_rad = lut(l, c, h, 2);
        float h_out_deg = h_out_rad * 180.0f / M_PI;

        printf("%-8s: (%6.2f, %6.2f, %6.1f) | (%6.2f, %6.2f, %7.1f deg)\n",
               label.c_str(), l_in, c_in, h_in, l_out, c_out, h_out_deg);
    };

    print_row(0, mid, mid, "Black");
    print_row(mid, 0, mid, "Gray");
    print_row(end, mid, mid, "White");
    print_row(mid, mid, mid, "Mid Sat");
    print_row(mid, end, mid, "High Sat");
    print_row(mid, mid, 0, "Red Hue");
    print_row(mid, mid, size/3, "Green Hue");
    printf("-----------------------------------------------------------------\n\n");
}

} // namespace


int main(int argc, char **argv) {
    SimpleTimer total_timer("Total Application Time");

    if (argc == 1) {
        print_usage();
        return 0;
    }

    // --- Argument Parsing using the new shared parser ---
    ProcessConfig cfg;
    try {
        cfg = parse_args(argc, argv);
    } catch (const std::runtime_error& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }

    if (cfg.input_path.empty() || cfg.output_path.empty()) {
        fprintf(stderr, "Error: --input and --output arguments are required for command-line processing.\n\n");
        print_usage();
        return 1;
    }

    // --- Load Input using the new raw_load module ---
    RawImageData raw_data;
    if (cfg.raw_png) {
        fprintf(stderr, "input (raw png): %s\n", cfg.input_path.c_str());
        raw_data = load_raw_png(cfg.input_path);
    } else {
        fprintf(stderr, "input (rawspeed): %s\n", cfg.input_path.c_str());
        raw_data = load_raw(cfg.input_path);
    }
    
    Buffer<uint16_t, 2> input = raw_data.bayer_data;
    int cfa_pattern = raw_data.cfa_pattern;
    int blackLevel = raw_data.black_level;
    int whiteLevel = raw_data.white_level;

    fprintf(stderr, "       %d %d\n", input.width(), input.height());


    // Convert string-based algorithm name to the integer ID the Halide pipeline expects
    int demosaic_id = 3; // default to 'fast'
    if (cfg.demosaic_algorithm == "ahd") demosaic_id = 0;
    else if (cfg.demosaic_algorithm == "lmmse") demosaic_id = 1;
    else if (cfg.demosaic_algorithm == "ri") demosaic_id = 2;
    else if (cfg.demosaic_algorithm != "fast") {
        std::cerr << "Warning: unknown demosaic algorithm '" << cfg.demosaic_algorithm << "'. Defaulting to fast.\n";
    }

    // Calculate output dimensions based on crop and downscale factor
    int out_width = static_cast<int>(input.width() / cfg.downscale_factor);
    int out_height = static_cast<int>(input.height() / cfg.downscale_factor);

    Buffer<float, 1> distortion_lut;
    {
        SimpleTimer lens_timer("Lens Correction LUT Generation");
        distortion_lut = PipelineUtils::LensCorrection::generate_identity_lut();
#ifdef USE_LENSFUN
        bool needs_lensfun = !cfg.camera_make.empty() && !cfg.camera_model.empty() &&
                             cfg.lens_profile_name != "None" && !cfg.lens_profile_name.empty();
        bool lensfun_applied = false;
        if (needs_lensfun) {
            fprintf(stderr, "Attempting to load Lensfun profile...\n");
            fprintf(stderr, "  Camera: %s %s\n", cfg.camera_make.c_str(), cfg.camera_model.c_str());
            fprintf(stderr, "  Lens: %s @ %.1fmm\n", cfg.lens_profile_name.c_str(), cfg.focal_length);

            lfDatabase ldb;
            ldb.Load();

            const lfCamera** cams = lf_db_find_cameras(&ldb, cfg.camera_make.c_str(), cfg.camera_model.c_str());
            if (!cams || !cams[0]) {
                fprintf(stderr, "  -> Warning: Camera '%s %s' not found in Lensfun database.\n", cfg.camera_make.c_str(), cfg.camera_model.c_str());
            } else {
                const lfCamera *cam = cams[0]; // Use the first match
                const lfLens** lenses = lf_db_find_lenses_hd(&ldb, cam, nullptr, cfg.lens_profile_name.c_str(), 0);
                if (!lenses || !lenses[0]) {
                    fprintf(stderr, "  -> Warning: Lens profile '%s' not found for specified camera.\n", cfg.lens_profile_name.c_str());
                } else {
                    const lfLens *lens = lenses[0]; // Use the best match
                    lfLensCalibDistortion distortion_model;
                    if (lens->InterpolateDistortion(cfg.focal_length, distortion_model)) {
                        const char* model_name = "Unknown";
                        if (distortion_model.Model == LF_DIST_MODEL_POLY3) model_name = "POLY3";
                        else if (distortion_model.Model == LF_DIST_MODEL_POLY5) model_name = "POLY5";
                        else if (distortion_model.Model == LF_DIST_MODEL_PTLENS) model_name = "PTLENS";

                        if (distortion_model.Model == LF_DIST_MODEL_POLY3 ||
                            distortion_model.Model == LF_DIST_MODEL_POLY5 ||
                            distortion_model.Model == LF_DIST_MODEL_PTLENS) {

                            fprintf(stderr, "  -> Profile loaded (%s). Generating inverse distortion LUT...\n", model_name);
                            distortion_lut = PipelineUtils::LensCorrection::generate_distortion_lut(distortion_model);
                            lensfun_applied = true;
                        } else {
                            fprintf(stderr, "  -> Warning: Lens profile uses an unsupported distortion model (%s). Distortion not applied.\n", model_name);
                        }
                    } else {
                        fprintf(stderr, "  -> Warning: Could not retrieve distortion params for this focal length.\n");
                    }
                }
                if (lenses) lf_free((void*)lenses);
            }
            if (cams) lf_free((void*)cams);
        }

        if (!lensfun_applied) {
            const float e = 1e-6f;
            if (fabsf(cfg.dist_k1) > e || fabsf(cfg.dist_k2) > e || fabsf(cfg.dist_k3) > e) {
                fprintf(stderr, "Using manual distortion parameters k1, k2, k3...\n");
                lfLensCalibDistortion manual_model;
                manual_model.Model = LF_DIST_MODEL_POLY5;
                manual_model.Terms[0] = cfg.dist_k1;
                manual_model.Terms[1] = cfg.dist_k2;
                distortion_lut = PipelineUtils::LensCorrection::generate_distortion_lut(manual_model);
            }
        }
#endif
    }
    
    Buffer<uint8_t, 3> output(out_width, out_height, 3);

    Buffer<uint16_t, 2> tone_curve_lut;
    Buffer<float, 4> color_grading_lut;
    {
        SimpleTimer lut_timer("Host LUT Generation");
        tone_curve_lut = ToneCurveUtils::generate_pipeline_lut(cfg);
        color_grading_lut = HostColor::generate_color_lut(cfg);
        print_lut_sample(color_grading_lut);
    }
    
    Buffer<float, 2> matrix_3200(4, 3), matrix_7000(4, 3);
    PipelineUtils::prepare_color_matrices(raw_data, matrix_3200, matrix_7000);

    if (raw_data.has_matrix) {
        fprintf(stderr, "Using color matrix from RAW file metadata.\n");
    } else {
        fprintf(stderr, "Using hardcoded DNG color matrices.\n");
    }

    float denoise_strength_norm = std::max(0.0f, std::min(1.0f, cfg.denoise_strength / 100.0f));
    float exposure_multiplier = powf(2.0f, cfg.exposure);

    // --- Halide Pipeline Execution and Benchmarking ---
    double best_time = std::numeric_limits<double>::infinity();
    for (int i = 0; i < cfg.timing_iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();
        #if defined(PIPELINE_PRECISION_F32)
            camera_pipe_f32(input, cfa_pattern, cfg.green_balance, cfg.downscale_factor, demosaic_id, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, exposure_multiplier, cfg.ca_strength,
                              denoise_strength_norm, cfg.denoise_eps,
                              blackLevel, whiteLevel, tone_curve_lut,
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
                              output);
        #elif defined(PIPELINE_PRECISION_U16)
            camera_pipe_u16(input, cfa_pattern, cfg.green_balance, cfg.downscale_factor, demosaic_id, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, exposure_multiplier, cfg.ca_strength,
                              denoise_strength_norm, cfg.denoise_eps,
                              blackLevel, whiteLevel, tone_curve_lut,
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
                              output);
        #endif
        output.device_sync();
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsed = end - start;
        if (elapsed.count() < best_time) {
            best_time = elapsed.count();
        }
    }

    if (getenv("HL_PROFILE") == nullptr) {
        #if defined(PIPELINE_PRECISION_F32)
            fprintf(stdout, "Using float32 pipeline.\n");
        #elif defined(PIPELINE_PRECISION_U16)
            fprintf(stdout, "Using uint16_t pipeline.\n");
        #endif
        fprintf(stdout, "Halide pipeline execution time: %f ms\n", best_time * 1000.0);
    }

#ifndef NO_AUTO_SCHEDULE
    // Auto-schedule benchmarking would go here
#endif
    
    {
        SimpleTimer save_timer("Image Save");
        fprintf(stderr, "output: %s\n", cfg.output_path.c_str());
        convert_and_save_image(output, cfg.output_path);
        fprintf(stderr, "        %d %d\n", output.width(), output.height());
    }

    std::string curve_png_path = cfg.output_path.substr(0, cfg.output_path.find_last_of('.')) + "_curve.png";
    if (ToneCurveUtils::render_curves_to_png(cfg, curve_png_path.c_str())) {
        fprintf(stderr, "curve:  %s\n", curve_png_path.c_str());
    }

    printf("Success!\n");
    return 0;
}
