#include "test_harness.h"

// Include all pipeline stages to test them
#include "stage_hot_pixel_suppression.h"
#include "stage_ca_correct.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h"
#include "stage_color_correct.h"
#include "stage_normalize_and_expose.h"
#include "stage_saturation.h"

// This test file was created to resolve compilation errors. It builds
// a portion of the pipeline to ensure the stage signatures and
// constructor calls are correct. It does not perform any assertions.
void test_stage_outputs() {
    std::cout << "--- Running test: test_stage_outputs (compilation check) ---\n";

    using namespace Halide;

    // Define common variables
    Var x("x"), y("y"), c("c");

    // Create a dummy input buffer for type-checking the pipeline
    const int width = 128, height = 128;
    Buffer<uint16_t> input = make_constant_buffer<uint16_t>({width, height}, 1024);
    ImageParam input_param(UInt(16), 2, "input_param_stages");
    input_param.set(input);

    Func shifted("shifted_dump");
    shifted(x, y) = input_param(x, y);

    Func shifted_clamped = BoundaryConditions::repeat_edge(shifted, {{0, width}, {0, height}});
    Func denoised = pipeline_hot_pixel_suppression(shifted_clamped, x, y);

    CACorrectBuilder ca_builder(denoised, x, y, 0.0f, 25.0f, 4095.0f, width, height, get_host_target(), false);
    Func ca_corrected = ca_builder.output;

    // Use placeholder values for the normalization stage
    Func normalized = pipeline_normalize_and_expose(ca_corrected, 25.0f, 4095.0f, 1.0f, x, y);

    Func deinterleaved = pipeline_deinterleave(normalized, x, y, c);

    // FIX: Call the DemosaicBuilder with the correct constructor signature,
    // providing the algorithm, width, and height.
    DemosaicBuilder demosaic_builder(deinterleaved, x, y, c, 1, width, height); // Use VHG
    Func demosaiced = demosaic_builder.output;

    Buffer<float> mat_buf(4, 3);
    mat_buf.fill(0.0f);
    mat_buf(0, 0) = 1.0f;
    mat_buf(1, 1) = 1.0f;
    mat_buf(2, 2) = 1.0f;

    // FIX: Add the missing 'white_point' argument to the function call.
    Func color_corrected = pipeline_color_correct(demosaiced, buffer_to_func(mat_buf, "m32"), buffer_to_func(mat_buf, "m70"), 3700.0f, 0.0f, 4095.0f, x, y, c, Halide::get_host_target(), false);
    Func saturated = pipeline_saturation(color_corrected, 1.0f, 1, x, y, c);

    // Realize the final stage to ensure the graph is valid.
    // We don't need to check the output, just that it compiles and runs.
    try {
        saturated.realize({width, height, 3});
        std::cout << "      SUCCESS: Pipeline graph in test_stage_outputs compiled and realized successfully.\n";
    } catch (const Halide::CompileError &e) {
        std::cerr << "Halide compile error in test_stage_outputs: " << e.what() << std::endl;
        test_failures++;
    } catch (const Halide::RuntimeError &e) {
        std::cerr << "Halide runtime error in test_stage_outputs: " << e.what() << std::endl;
        test_failures++;
    }
}
