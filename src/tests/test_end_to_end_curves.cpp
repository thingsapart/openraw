#include "test_harness.h"

#include "pipeline_helpers.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h"
#include "stage_color_correct.h"
#include "stage_normalize_and_expose.h"
#include "stage_saturation.h"
#include "stage_apply_curve.h"
#include "tone_curve_utils.h"

// Helper function to run the common linear part of the pipeline.
// NOTE: It now returns the float-valued Func right before the LUT is applied.
Halide::Func run_linear_pipeline_for_test(Halide::Func raw_func, int raw_width, int raw_height) {
    using namespace Halide;
    Var x, y, c;

    // Use default normalization parameters for testing
    Expr black_point = 0.0f, white_point = 65535.0f, exposure = 1.0f;
    Func normalized = pipeline_normalize_and_expose(raw_func, black_point, white_point, exposure, x, y);

    Func deinterleaved = pipeline_deinterleave(normalized, x, y, c);
    DemosaicBuilder demosaic_builder(deinterleaved, x, y, c, 0, raw_width, raw_height);
    Func demosaiced = demosaic_builder.output;
    
    // Use identity color matrices for a transparent color stage
    Buffer<float> matrix_buf(4, 3);
    matrix_buf.fill(0.0f);
    matrix_buf(0,0) = 1.0f;
    matrix_buf(1,1) = 1.0f;
    matrix_buf(2,2) = 1.0f;
    Func matrix_func = buffer_to_func(matrix_buf, "identity_matrix");

    Func color_corrected = pipeline_color_correct(demosaiced, matrix_func, matrix_func, 5000.0f, 0.0f, white_point, x, y, c, get_host_target(), false);
    return pipeline_saturation(color_corrected, 1.0f, 1, x, y, c);
}


void test_e2e_linear_curve() {
    std::cout << "--- Running test: test_e2e_linear_curve ---\n";
    Halide::Var x, y, c;

    const int W = 64, H = 64;
    Halide::Buffer<uint16_t> raw_buffer(W, H);
    raw_buffer.for_each_element([&](int ix, int iy) {
        raw_buffer(ix, iy) = (iy * W + ix) * 16; // Gradient from 0 to 65520
    });
    Halide::Func raw_func = buffer_to_func(raw_buffer, "raw_gradient");

    // 1. Run the linear part of the pipeline to get the float data.
    Halide::Func linear_float_output = run_linear_pipeline_for_test(raw_func, W, H);

    // 2. Convert to u16 for LUT application, this is our "expected" input to the curve.
    Halide::Func to_uint16("to_uint16_test");
    to_uint16(x, y, c) = Halide::cast<uint16_t>(Halide::clamp(linear_float_output(x, y, c) * 65535.f, 0.f, 65535.f));

    // 3. Configure a 1-to-1 linear curve.
    ProcessConfig cfg;
    cfg.curve_points_str = "0:0,1:1";
    cfg.tonemap_algorithm = 0; // Linear tonemap
    ToneCurveUtils util(cfg);
    Halide::Buffer<uint8_t> lut_buffer(util.get_lut_for_halide());
    Halide::Func lut_func = buffer_to_func(lut_buffer, "linear_lut");

    // 4. Apply the curve to the u16 data.
    Halide::Func curved = pipeline_apply_curve(to_uint16, lut_func, lut_buffer.width(), 1, x, y, c);
    Halide::Buffer<uint8_t> actual_output = curved.realize({W, H, 3});

    // 5. Assert that the output is the 8-bit equivalent of the 16-bit input.
    Halide::Buffer<uint16_t> expected_input_u16 = to_uint16.realize({W, H, 3});
    actual_output.for_each_element([&](int ix, int iy, int ic) {
        uint8_t expected_val = expected_input_u16(ix, iy, ic) >> 8;
        ASSERT_NEAR(actual_output(ix, iy, ic), expected_val, 1);
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

    Halide::Func linear_float_output = run_linear_pipeline_for_test(raw_func, W, H);
    Halide::Func to_uint16("to_uint16_inv_test");
    to_uint16(x, y, c) = Halide::cast<uint16_t>(Halide::clamp(linear_float_output(x, y, c) * 65535.f, 0.f, 65535.f));
    Halide::Buffer<uint16_t> linear_output_u16 = to_uint16.realize({W, H, 3});

    ProcessConfig cfg;
    cfg.curve_points_str = "0:1,1:0";
    cfg.tonemap_algorithm = 0; // Linear tonemap
    ToneCurveUtils util(cfg);
    Halide::Buffer<uint8_t> lut_buffer(util.get_lut_for_halide());
    Halide::Func lut_func = buffer_to_func(lut_buffer, "inverting_lut");

    Halide::Func curved = pipeline_apply_curve(to_uint16, lut_func, lut_buffer.width(), 1, x, y, c);
    Halide::Buffer<uint8_t> actual_output = curved.realize({W, H, 3});

    actual_output.for_each_element([&](int ix, int iy, int ic) {
        uint8_t expected_val = (65535 - linear_output_u16(ix, iy, ic)) >> 8;
        ASSERT_NEAR(actual_output(ix, iy, ic), expected_val, 1);
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

    Halide::Func linear_float_output = run_linear_pipeline_for_test(raw_func, W, H);
    Halide::Func to_uint16("to_uint16_crush_test");
    to_uint16(x, y, c) = Halide::cast<uint16_t>(Halide::clamp(linear_float_output(x, y, c) * 65535.f, 0.f, 65535.f));
    
    ProcessConfig cfg;
    cfg.curve_points_str = "0:0,0.25:0,0.75:1,1:1";
    cfg.tonemap_algorithm = 0; // Linear tonemap
    ToneCurveUtils util(cfg);
    Halide::Buffer<uint8_t> lut_buffer(util.get_lut_for_halide());
    Halide::Func lut_func = buffer_to_func(lut_buffer, "crushing_lut");

    Halide::Func curved = pipeline_apply_curve(to_uint16, lut_func, lut_buffer.width(), 1, x, y, c);
    Halide::Buffer<uint8_t> actual_output = curved.realize({W, H, 3});

    // Check the first 20% of pixels (should be black)
    for (int iy = 0; iy < 12; ++iy) {
        for (int ix = 0; ix < W; ++ix) {
            ASSERT_TRUE(actual_output(ix, iy, 0) < 2);
        }
    }

    // Check the last 20% of pixels (should be white)
    for (int iy = 52; iy < H; ++iy) {
        for (int ix = 0; ix < W; ++ix) {
            ASSERT_TRUE(actual_output(ix, iy, 0) > 253);
        }
    }
}
