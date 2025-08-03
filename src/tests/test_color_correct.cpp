#include "test_harness.h"
#include "stage_color_correct.h"

void test_color_correct_behavior() {
    std::cout << "--- Running test: test_color_correct_behavior ---\n";
    Halide::Var x, y, c;

    // Create color matrix buffers. We only test the 3200K matrix for worst-case blue shift.
    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f}, {-0.3576f, 1.0615f, 1.5949f, -37.1158f}, {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};
    Halide::Buffer<float> matrix_buf(4, 3);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) {
        matrix_buf(j, i) = _matrix_3200[i][j];
    }
    Halide::Func matrix_func = buffer_to_func(matrix_buf, "mat3200_cc_behavior");

    // The test inputs are already floats in a specific numeric range, not normalized data.
    // To make the color matrix math work, we pass a "white_point" of 1.0f, which effectively
    // makes the normalization of the matrix offset a no-op (division by 1).
    Halide::Expr test_white_point = 1.0f;

    // Case 1: Absolute Black (should clamp to zero, not wrap around)
    {
        auto input_buffer = make_constant_buffer<float>({2, 2, 3}, 0.0f);
        Halide::Func input_func = buffer_to_func(input_buffer, "cc_black_input");
        Halide::Func corrected = pipeline_color_correct(input_func, matrix_func, matrix_func, 7000.f, 0.f, test_white_point, x, y, c, Halide::get_host_target(), false);
        Halide::Func clamped = Halide::Func("clamped_black");
        clamped(x,y,c) = Halide::cast<uint16_t>(Halide::clamp(corrected(x,y,c), 0.f, 65535.f));
        Halide::Buffer<uint16_t> output = clamped.realize({2, 2, 3});
        
        // The negative offset in the matrix should be clamped to 0.
        output.for_each_value([](uint16_t& val) {
            ASSERT_EQUAL(val, 0);
        });
    }

    // Case 2: Near Black (should also clamp to zero for R, but be positive for G, B)
    {
        auto input_buffer = make_constant_buffer<float>({2, 2, 3}, 20.0f);
        Halide::Func input_func = buffer_to_func(input_buffer, "cc_near_black_input");
        Halide::Func corrected = pipeline_color_correct(input_func, matrix_func, matrix_func, 7000.f, 0.f, test_white_point, x, y, c, Halide::get_host_target(), false);
        Halide::Func clamped = Halide::Func("clamped_near_black");
        clamped(x,y,c) = Halide::cast<uint16_t>(Halide::clamp(corrected(x,y,c), 0.f, 65535.f));
        Halide::Buffer<uint16_t> output = clamped.realize({2, 2, 3});

        // The matrix math will result in a negative value for red, but positive for G and B.
        output.for_each_element([&](int ix, int iy, int ic) {
            if (ic == 0) ASSERT_EQUAL(output(ix, iy, ic), 0); // R should be 0
            if (ic == 1) ASSERT_EQUAL(output(ix, iy, ic), 8); // G should be 8
            if (ic == 2) ASSERT_EQUAL(output(ix, iy, ic), 70); // B should be 70
        });
    }

    // Case 3: Bright White (should clamp to 65535)
    {
        auto input_buffer = make_constant_buffer<float>({2, 2, 3}, 15000.0f);
        Halide::Func input_func = buffer_to_func(input_buffer, "cc_white_input");
        Halide::Func corrected = pipeline_color_correct(input_func, matrix_func, matrix_func, 7000.f, 0.f, test_white_point, x, y, c, Halide::get_host_target(), false);
        Halide::Func clamped = Halide::Func("clamped_white");
        clamped(x,y,c) = Halide::cast<uint16_t>(Halide::clamp(corrected(x,y,c), 0.f, 65535.f));
        Halide::Buffer<uint16_t> output = clamped.realize({2, 2, 3});

        ASSERT_TRUE(output(0,0,0) < 65535); // R should not clip
        ASSERT_TRUE(output(0,0,1) < 65535); // G should not clip
        ASSERT_EQUAL(output(0,0,2), 65535); // B should clip
    }
}
