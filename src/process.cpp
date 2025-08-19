#include <cstdint>
#include <string>
#include <map>
#include <stdexcept>
#include <iostream>
#include <cstdlib> // For getenv
#include <chrono>  // For timing
#include <limits>  // For numeric_limits
#include <cmath>   // for powf
#include <iomanip> // for std::setprecision

// To enable Lensfun support, compile with -DUSE_LENSFUN and link against liblensfun.
#ifdef USE_LENSFUN
#include "lensfun/lensfun.h"
#endif

#include "HalideBuffer.h"
#include "halide_image_io.h"
#include "halide_malloc_trace.h"
#include "tone_curve_utils.h"
#include "process_options.h" // Use the new shared header
#include "color_tools.h"     // For LUT generation

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


int main(int argc, char **argv) {
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

    // Convert string-based algorithm name to the integer ID the Halide pipeline expects
    int demosaic_id = 3; // default to 'fast'
    if (cfg.demosaic_algorithm == "ahd") demosaic_id = 0;
    else if (cfg.demosaic_algorithm == "lmmse") demosaic_id = 1;
    else if (cfg.demosaic_algorithm == "ri") demosaic_id = 2;
    else if (cfg.demosaic_algorithm != "fast") {
        std::cerr << "Warning: unknown demosaic algorithm '" << cfg.demosaic_algorithm << "'. Defaulting to fast.\n";
    }

    fprintf(stderr, "input: %s\n", cfg.input_path.c_str());
    Buffer<uint16_t, 2> input = load_and_convert_image(cfg.input_path);
    fprintf(stderr, "       %d %d\n", input.width(), input.height());

    // Calculate output dimensions based on crop and downscale factor
    int out_width = static_cast<int>((input.width() - 32) / cfg.downscale_factor);
    int out_height = static_cast<int>((input.height() - 24) / cfg.downscale_factor);

#ifdef USE_LENSFUN
    bool needs_lensfun = !cfg.camera_make.empty() && !cfg.camera_model.empty() &&
                         cfg.lens_profile_name != "None" && !cfg.lens_profile_name.empty();
    if (needs_lensfun) {
        fprintf(stderr, "Attempting to load Lensfun profile...\n");
        fprintf(stderr, "  Camera: %s %s\n", cfg.camera_make.c_str(), cfg.camera_model.c_str());
        fprintf(stderr, "  Lens: %s @ %.1fmm\n", cfg.lens_profile_name.c_str(), cfg.focal_length);

        lfDatabase *ldb = lf_db_new();
        if (!ldb) {
            fprintf(stderr, "  -> Error: Could not create Lensfun database object.\n");
        } else {
            lf_db_load(ldb);

            const lfCamera **cams = lf_db_find_cameras(ldb, cfg.camera_make.c_str(), cfg.camera_model.c_str());
            if (!cams || !cams[0]) {
                fprintf(stderr, "  -> Warning: Camera '%s %s' not found in Lensfun database.\n", cfg.camera_make.c_str(), cfg.camera_model.c_str());
            } else {
                const lfCamera *cam = cams[0]; // Use the first match

                // Use lf_db_find_lenses_hd to find the lens by its model name, for the given camera
                const lfLens **lenses = lf_db_find_lenses_hd(ldb, cam, nullptr, cfg.lens_profile_name.c_str(), 0);

                if (!lenses || !lenses[0]) {
                    fprintf(stderr, "  -> Warning: Lens profile '%s' not found for specified camera.\n", cfg.lens_profile_name.c_str());
                } else {
                    const lfLens *lens = lenses[0]; // Use the best match

                    // Interpolate distortion parameters for the given focal length
                    lfLensCalibDistortion distortion_model;
                    if (lf_lens_interpolate_distortion(lens, cfg.focal_length, &distortion_model)) {
                        if (distortion_model.Model == LF_DIST_MODEL_POLY5) {
                            // This model is compatible with the Halide pipeline's k1 and k2 terms.
                            if (cfg.dist_k1 == UNSET_F) cfg.dist_k1 = distortion_model.Terms[0];
                            if (cfg.dist_k2 == UNSET_F) cfg.dist_k2 = distortion_model.Terms[1];
                            if (cfg.dist_k3 == UNSET_F) cfg.dist_k3 = 0.0f; // POLY5 has no k3 term.
                            fprintf(stderr, "  -> Profile loaded (POLY5). Using Distortion(k1,k2,k3): %.5f, %.5f, %.5f\n",
                                    cfg.dist_k1, cfg.dist_k2, cfg.dist_k3);

                        } else if (distortion_model.Model == LF_DIST_MODEL_POLY3) {
                            // The Halide pipeline's model is a pure polynomial in r^2, r^4, r^6.
                            // The lensfun.h header for POLY3 is r_d = r_u * (1 - k1 + k1 * r_u^2), which is not compatible.
                            fprintf(stderr, "  -> Warning: Lens profile uses POLY3 distortion model, which is incompatible with this pipeline's math. Distortion not applied.\n");

                        } else if (distortion_model.Model == LF_DIST_MODEL_PTLENS) {
                            // PTLENS model is also not a pure even-power polynomial and is incompatible.
                            fprintf(stderr, "  -> Warning: Lens profile uses PTLENS distortion model, which is incompatible with this pipeline's math. Distortion not applied.\n");
                        } else {
                            fprintf(stderr, "  -> Warning: Lens profile uses an unknown or unsupported distortion model. Distortion not applied.\n");
                        }

                    } else {
                        fprintf(stderr, "  -> Warning: Could not retrieve distortion params for this focal length.\n");
                    }
                    // TODO: Interpolate TCA and Vignetting when pipeline supports their specific models.
                }

                if (lenses) {
                    lf_free((void*)lenses);
                }
            }

            if (cams) {
                lf_free((void*)cams);
            }
            lf_db_destroy(ldb);
        }
    }
#endif

    // If any distortion params are still unset (either by user or lensfun), default them to 0.
    if (cfg.dist_k1 == UNSET_F) cfg.dist_k1 = 0.0f;
    if (cfg.dist_k2 == UNSET_F) cfg.dist_k2 = 0.0f;
    if (cfg.dist_k3 == UNSET_F) cfg.dist_k3 = 0.0f;

    Buffer<uint8_t, 3> output(out_width, out_height, 3);

    Buffer<uint16_t, 2> tone_curve_lut = ToneCurveUtils::generate_pipeline_lut(cfg);
    Buffer<float, 4> color_grading_lut = HostColor::generate_color_lut(cfg);

    // Print a sample of the LUT for debugging.
    print_lut_sample(color_grading_lut);

    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f},
                               {-0.3576f, 1.0615f, 1.5949f, -37.1158f},
                               {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};
    float _matrix_7000[][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f},
                               {-0.3826f, 1.5906f, -0.2080f, -25.4311f},
                               {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};
    Buffer<float, 2> matrix_3200(4, 3), matrix_7000(4, 3);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            matrix_3200(j, i) = _matrix_3200[i][j];
            matrix_7000(j, i) = _matrix_7000[i][j];
        }
    }

    int blackLevel = 25;
    int whiteLevel = 1023;
    float denoise_strength_norm = std::max(0.0f, std::min(1.0f, cfg.denoise_strength / 100.0f));
    float exposure_multiplier = powf(2.0f, cfg.exposure);

    // --- Simple Timing/Profiling Loop ---
    double best_time = std::numeric_limits<double>::infinity();
    for (int i = 0; i < cfg.timing_iterations; i++) {
        auto start = std::chrono::high_resolution_clock::now();

        #if defined(PIPELINE_PRECISION_F32)
            camera_pipe_f32(input, cfg.downscale_factor, demosaic_id, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, exposure_multiplier, cfg.ca_strength,
                              denoise_strength_norm, cfg.denoise_eps,
                              blackLevel, whiteLevel, tone_curve_lut,
                              0.f, 0.f, 0.f, /* sharpen */
                              cfg.ll_detail, cfg.ll_clarity, cfg.ll_shadows, cfg.ll_highlights, cfg.ll_blacks, cfg.ll_whites,
                              color_grading_lut,
                              cfg.vignette_amount, cfg.vignette_midpoint, cfg.vignette_roundness, cfg.vignette_highlights,
                              cfg.dehaze_strength,
                              cfg.ca_red_cyan, cfg.ca_blue_yellow,
                              cfg.dist_k1, cfg.dist_k2, cfg.dist_k3,
                              cfg.geo_rotate, cfg.geo_scale, cfg.geo_aspect,
                              cfg.geo_keystone_v, cfg.geo_keystone_h,
                              cfg.geo_offset_x, cfg.geo_offset_y,
                              output);
        #elif defined(PIPELINE_PRECISION_U16)
            camera_pipe_u16(input, cfg.downscale_factor, demosaic_id, matrix_3200, matrix_7000,
                              cfg.color_temp, cfg.tint, exposure_multiplier, cfg.ca_strength,
                              denoise_strength_norm, cfg.denoise_eps,
                              blackLevel, whiteLevel, tone_curve_lut,
                              0.f, 0.f, 0.f, /* sharpen */
                              cfg.ll_detail, cfg.ll_clarity, cfg.ll_shadows, cfg.ll_highlights, cfg.ll_blacks, cfg.ll_whites,
                              color_grading_lut,
                              cfg.vignette_amount, cfg.vignette_midpoint, cfg.vignette_roundness, cfg.vignette_highlights,
                              cfg.dehaze_strength,
                              cfg.ca_red_cyan, cfg.ca_blue_yellow,
                              cfg.dist_k1, cfg.dist_k2, cfg.dist_k3,
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

    // The profiler will print its report to stderr automatically when the program
    // exits, so we just need to print our own timing results.
    // Check if profiling was enabled to avoid printing benchmark time when it's not relevant.
    if (getenv("HL_PROFILE") == nullptr) {
        #if defined(PIPELINE_PRECISION_F32)
            fprintf(stderr, "Using float32 pipeline.\n");
        #elif defined(PIPELINE_PRECISION_U16)
            fprintf(stderr, "Using uint16_t pipeline.\n");
        #endif
        fprintf(stderr, "Halide (manual):\t%gus\n", best_time * 1e6);
    }

#ifndef NO_AUTO_SCHEDULE
    // Auto-schedule benchmarking would go here, with a similar if/else structure
#endif

    fprintf(stderr, "output: %s\n", cfg.output_path.c_str());
    convert_and_save_image(output, cfg.output_path);
    fprintf(stderr, "        %d %d\n", output.width(), output.height());

    std::string curve_png_path = cfg.output_path.substr(0, cfg.output_path.find_last_of('.')) + "_curve.png";
    if (ToneCurveUtils::render_curves_to_png(cfg, curve_png_path.c_str())) {
        fprintf(stderr, "curve:  %s\n", curve_png_path.c_str());
    }

    printf("Success!\n");
    return 0;
}
