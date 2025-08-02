#include "test_harness.h"

#include "pipeline_helpers.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h"
#include "stage_color_correct.h"
#include "stage_exposure.h"
#include "stage_saturation.h"
#include "stage_apply_curve.h"
#include "tone_curve_utils.h"

// Helper function to run the common linear part of the pipeline
Halide::Func run_linear_pipeline(Halide::Func raw_func, int raw_width, int raw_height) {
    using namespace Halide;
    Var x, y, c;

    // Convert raw to float for processing
    Func to_float("to_float_test");
    to_float(x, y) = cast<float>(raw_func(x, y));

    Func deinterleaved = pipeline_deinterleave(to_float, x, y, c);
    DemosaicBuilder demosaic_builder(deinterleaved, x, y, c, 0, raw_width / 2, raw_height / 2);
    Func demosaiced = demosaic_builder.output;
    
    // Use identity color matrices for a transparent color stage
    Buffer<float> matrix_buf(4, 3);
    matrix_buf.fill(0.0f);
    matrix_buf(0,0) = 1.0f;
    matrix_buf(1,1) = 1.0f;
    matrix_buf(2,2) = 1.0f;
    Func matrix_func = buffer_to_func(matrix_buf, "identity_matrix");

    Func color_corrected = pipeline_color_correct(demosaiced, matrix_func, matrix_func, 5000.0f, 0.0f, x, y, c, get_host_target(), false);
    Func exposed = pipeline_exposure(color_corrected, 1.0f, x, y, c);
    Func saturated = pipeline_saturation(exposed, 1.0f, 1, x, y, c);
    
    // Convert back to uint16 for the curve stage
    Func to_uint16("to_uint16_test");
    to_uint16(x, y, c) = cast<uint16_t>(clamp(saturated(x, y, c), 0.f, 65535.f));

    return to_uint16;
}


void test_e2e_linear_curve() {
    std::cout << "--- Running test: test_e2e_linear_curve ---\n";
    Halide::Var x, y, c;

    // 1. Create a synthetic RAW buffer with a smooth gradient
    const int W = 64, H = 64;
    Halide::Buffer<uint16_t> raw_buffer(W, H);
    raw_buffer.for_each_element([&](int ix, int iy) {
        raw_buffer(ix, iy) = (iy * W + ix) * 16; // Gradient from 0 to 65520
    });
    Halide::Func raw_func = buffer_to_func(raw_buffer, "raw_gradient");

    // 2. Run the linear part of the pipeline to get our "expected" output
    Halide::Func linear_output_func = run_linear_pipeline(raw_func, W, H);
    Halide::Buffer<uint16_t> expected_output = linear_output_func.realize({W, H, 3});

    // 3. Configure a 1-to-1 linear curve
    ProcessConfig cfg;
    cfg.curve_points_str = "0:0,1:1";
    ToneCurveUtils util(cfg);
    // FIX: Explicitly wrap the Runtime::Buffer in a Halide::Buffer
    Halide::Buffer<uint16_t> lut_buffer(util.get_lut_for_halide());
    Halide::Func lut_func = buffer_to_func(lut_buffer, "linear_lut");

    // 4. Apply the curve to the linear output
    Halide::Func curved = pipeline_apply_curve(linear_output_func, lut_func, lut_buffer.width(), 1, x, y, c);
    Halide::Buffer<uint16_t> actual_output = curved.realize({W, H, 3});

    // 5. Assert that the output is nearly identical to the input of the curve stage
    actual_output.for_each_element([&](int ix, int iy, int ic) {
        ASSERT_NEAR(actual_output(ix, iy, ic), expected_output(ix, iy, ic), 2);
    });
}

void test_e2e_inverting_curve() {
    std::cout << "--- Running test: test_e2e_inverting_curve ---\n";
    Halide::Var x, y, c;

    const int W = 64, H = 64;
    Halide::Buffer<uint16_t> raw_buffer(W, H);
    raw_buffer.for_each_element([&](int ix, int iy) {
        raw_buffer(ix, iy) = (iy * W + ix) * 16;
    });
    Halide::Func raw_func = buffer_to_func(raw_buffer, "raw_gradient_inv");

    Halide::Func linear_output_func = run_linear_pipeline(raw_func, W, H);
    Halide::Buffer<uint16_t> linear_output = linear_output_func.realize({W, H, 3});

    ProcessConfig cfg;
    cfg.curve_points_str = "0:1,1:0";
    ToneCurveUtils util(cfg);
    // FIX: Explicitly wrap the Runtime::Buffer in a Halide::Buffer
    Halide::Buffer<uint16_t> lut_buffer(util.get_lut_for_halide());
    Halide::Func lut_func = buffer_to_func(lut_buffer, "inverting_lut");

    Halide::Func curved = pipeline_apply_curve(linear_output_func, lut_func, lut_buffer.width(), 1, x, y, c);
    Halide::Buffer<uint16_t> actual_output = curved.realize({W, H, 3});

    actual_output.for_each_element([&](int ix, int iy, int ic) {
        uint16_t expected_val = 65535 - linear_output(ix, iy, ic);
        ASSERT_NEAR(actual_output(ix, iy, ic), expected_val, 2);
    });
}

void test_e2e_crushing_curve() {
    std::cout << "--- Running test: test_e2e_crushing_curve ---\n";
    Halide::Var x, y, c;

    const int W = 64, H = 64;
    Halide::Buffer<uint16_t> raw_buffer(W, H);
    raw_buffer.for_each_element([&](int ix, int iy) {
        raw_buffer(ix, iy) = (iy * W + ix) * 16;
    });
    Halide::Func raw_func = buffer_to_func(raw_buffer, "raw_gradient_crush");

    Halide::Func linear_output_func = run_linear_pipeline(raw_func, W, H);
    
    ProcessConfig cfg;
    cfg.curve_points_str = "0:0,0.25:0,0.75:1,1:1";
    ToneCurveUtils util(cfg);
    // FIX: Explicitly wrap the Runtime::Buffer in a Halide::Buffer
    Halide::Buffer<uint16_t> lut_buffer(util.get_lut_for_halide());
    Halide::Func lut_func = buffer_to_func(lut_buffer, "crushing_lut");

    Halide::Func curved = pipeline_apply_curve(linear_output_func, lut_func, lut_buffer.width(), 1, x, y, c);
    Halide::Buffer<uint16_t> actual_output = curved.realize({W, H, 3});

    // Check the first 20% of pixels (should be black)
    for (int iy = 0; iy < 12; ++iy) {
        for (int ix = 0; ix < W; ++ix) {
            ASSERT_TRUE(actual_output(ix, iy, 0) < 10);
        }
    }

    // Check the last 20% of pixels (should be white)
    for (int iy = 52; iy < H; ++iy) {
        for (int ix = 0; ix < W; ++ix) {
            ASSERT_TRUE(actual_output(ix, iy, 0) > 65525);
        }
    }
}
