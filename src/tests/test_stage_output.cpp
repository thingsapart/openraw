#include "test_harness.h"
#include "halide_image_io.h"
#include <sys/stat.h>

// Include all the stages needed
#include "stage_hot_pixel_suppression.h"
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h"
#include "stage_color_correct.h"
#include "stage_exposure.h"
#include "stage_saturation.h"
#include "stage_apply_curve.h"

// Helper to check if a file exists
inline bool file_exists(const std::string& name) {
    struct stat buffer;
    return (stat(name.c_str(), &buffer) == 0);
}

// Helper to save the 16-bit, 3-channel (RGB) output of a stage
void dump_stage_output_rgb(Halide::Func stage, const std::string& filename, int W, int H) {
    std::cout << "Dumping RGB output of stage '" << stage.name() << "' to " << filename << "..." << std::endl;
    try {
        Halide::Buffer<uint16_t> output = stage.realize({W, H, 3});
        Halide::Tools::save_image(output, filename);
        std::cout << "  ...success." << std::endl;
    } catch (Halide::RuntimeError &e) {
        std::cerr << "  ...Halide runtime error while realizing stage: " << e.what() << std::endl;
        test_failures++;
    } catch (std::exception &e) {
        std::cerr << "  ...Error saving image: " << e.what() << std::endl;
        test_failures++;
    }
}

// Helper to save the 16-bit, single-channel (bayer) output of a stage
void dump_stage_output_bayer(Halide::Func stage, const std::string& filename, int W, int H) {
    std::cout << "Dumping Bayer output of stage '" << stage.name() << "' to " << filename << "..." << std::endl;
    try {
        Halide::Buffer<uint16_t> output = stage.realize({W, H});
        Halide::Tools::save_image(output, filename);
        std::cout << "  ...success." << std::endl;
    } catch (Halide::RuntimeError &e) {
        std::cerr << "  ...Halide runtime error while realizing stage: " << e.what() << std::endl;
        test_failures++;
    } catch (std::exception &e) {
        std::cerr << "  ...Error saving image: " << e.what() << std::endl;
        test_failures++;
    }
}


void test_dump_intermediate_stages() {
    std::cout << "--- Running test: test_dump_intermediate_stages ---\n";
    
    // !!! IMPORTANT !!!
    // Replace this with the actual path to a RAW .png file that shows the problem.
    const std::string input_raw_path = "raw_image.png";

    if (!file_exists(input_raw_path)) {
        std::cerr << "Could not find test image at: '" << input_raw_path << "'\n"
                  << "Skipping test_dump_intermediate_stages. Please provide a valid RAW png." << std::endl;
        return;
    }

    Halide::Buffer<uint16_t> input = Halide::Tools::load_image(input_raw_path);
    const int out_width = ((input.width() - 32) / 32) * 32;
    const int out_height = ((input.height() - 24) / 32) * 32;

    Halide::Var x("x"), y("y"), c("c");
    
    // --- Define the full pipeline graph ---
    
    Func shifted("shifted_dump");
    shifted(x, y) = input(x + 16, y + 12);
    
    Func shifted_clamped = Halide::BoundaryConditions::repeat_edge(shifted, {{0, out_width}, {0, out_height}});

    Func denoised = pipeline_hot_pixel_suppression(shifted_clamped, x, y);
    
    CACorrectBuilder ca_builder(denoised, x, y, 1.0f, 1, 4095, out_width, out_height, Halide::get_host_target(), false);
    Func ca_corrected = ca_builder.output;

    // --- FIX FOR HANG ---
    // The default JIT schedule tries to inline everything, which can hang the compiler
    // on a complex graph like the CA-correction stage. By explicitly adding
    // compute_root() to its internal funcs, we force them to be computed and
    // stored, simplifying the problem for the compiler.
    std::cout << "Applying debug schedules to CA-Correct intermediates to prevent hang..." << std::endl;
    for (Halide::Func& f : ca_builder.intermediates) {
        if (f.defined()) {
            f.compute_root();
        }
    }
    // --- END FIX ---

    Func deinterleaved = pipeline_deinterleave(ca_corrected, x, y, c);
    
    Func deinterleaved_clamped = Halide::BoundaryConditions::repeat_edge(deinterleaved, {{0, out_width/2}, {0, out_height/2}, {0, 4}});
    DemosaicBuilder demosaic_builder(deinterleaved_clamped, x, y, c);
    Func demosaiced = demosaic_builder.output;

    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f}, {-0.3576f, 1.0615f, 1.5949f, -37.1158f}, {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};
    float _matrix_7000[][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f}, {-0.3826f, 1.5906f, -0.2080f, -25.4311f}, {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};
    Halide::Buffer<float> mat3200(4, 3), mat7000(4, 3);
    for (int i=0; i<3; ++i) for(int j=0; j<4; ++j) { mat3200(j,i) = _matrix_3200[i][j]; mat7000(j,i) = _matrix_7000[i][j]; }
    
    Func color_corrected = pipeline_color_correct(demosaiced, buffer_to_func(mat3200, "m32"), buffer_to_func(mat7000, "m70"), 3700.0f, 0.0f, x, y, c, Halide::get_host_target(), false);
    
    Func exposed = pipeline_exposure(color_corrected, 1.0f, x, y, c);

    Func saturated = pipeline_saturation(exposed, 1.0f, 1, x, y, c);

    // --- Realize and dump each major intermediate Func ---
    // Note: The bayer-pattern stages are saved as single-channel (grayscale) PNGs.
    dump_stage_output_bayer(ca_corrected, "_dump_01_ca_corrected_bayer.png", out_width, out_height);
    dump_stage_output_rgb(demosaiced, "_dump_02_demosaiced_rgb.png", out_width, out_height);
    dump_stage_output_rgb(color_corrected, "_dump_03_color_corrected_rgb.png", out_width, out_height);
    dump_stage_output_rgb(exposed, "_dump_04_exposed_rgb.png", out_width, out_height);
    dump_stage_output_rgb(saturated, "_dump_05_saturated_rgb.png", out_width, out_height);
}
