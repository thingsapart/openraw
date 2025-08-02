#include "test_harness.h"

#include "pipeline_helpers.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h"
#include "stage_color_correct.h"
#include "stage_exposure.h"

void test_full_linear_pipeline() {
    std::cout << "--- Running test: test_full_linear_pipeline ---\n";
    Halide::Var x, y, c;

    // 1. Create a synthetic 4x4 RAW buffer with a gray value
    auto raw_buffer = make_constant_buffer<uint16_t>({4, 4}, 1000);
    Halide::Func raw_func = buffer_to_func(raw_buffer, "raw_func");
    Halide::Func raw_func_f("raw_func_f");
    raw_func_f(x, y) = Halide::cast<float>(raw_func(x,y));

    // 2. Run the pipeline in the corrected order
    Halide::Func deinterleaved = pipeline_deinterleave(raw_func_f, x, y, c);
    DemosaicBuilder demosaic_builder(deinterleaved, x, y, c, 0, 2, 2);
    Halide::Func demosaiced = demosaic_builder.output;
    
    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f}, {-0.3576f, 1.0615f, 1.5949f, -37.1158f}, {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};
    float _matrix_7000[][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f}, {-0.3826f, 1.5906f, -0.2080f, -25.4311f}, {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};
    Halide::Buffer<float> matrix_buf_3200(4, 3), matrix_buf_7000(4, 3);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) {
        matrix_buf_3200(j, i) = _matrix_3200[i][j];
        matrix_buf_7000(j, i) = _matrix_7000[i][j];
    }
    Halide::Func matrix_func_3200 = buffer_to_func(matrix_buf_3200, "mat3200_full");
    Halide::Func matrix_func_7000 = buffer_to_func(matrix_buf_7000, "mat7000_full");

    Halide::Func color_corrected = pipeline_color_correct(demosaiced, matrix_func_3200, matrix_func_7000, 5000.0f, 0.0f, x, y, c, Halide::get_host_target(), false);
    Halide::Func exposed = pipeline_exposure(color_corrected, 2.5f, x, y, c);

    Halide::Func to_uint16("to_uint16_final");
    to_uint16(x, y, c) = Halide::cast<uint16_t>(Halide::clamp(exposed(x, y, c), 0.f, 65535.f));

    Halide::Buffer<uint16_t> output = to_uint16.realize({4, 4, 3});

    // 3. Calculate expected output
    float color_temp = 5000.0f;
    float alpha = (1.0f / color_temp - 1.0f / 3200.f) / (1.0f / 7000.f - 1.0f / 3200.f);
    float m[3][4];
    for (int i=0; i<3; ++i) for (int j=0; j<4; ++j) {
        m[i][j] = matrix_buf_3200(j,i) * alpha + matrix_buf_7000(j,i) * (1-alpha);
    }
    float ir=1000, ig=1000, ib=1000;
    float r_cc = m[0][0]*ir + m[0][1]*ig + m[0][2]*ib + m[0][3];
    float g_cc = m[1][0]*ir + m[1][1]*ig + m[1][2]*ib + m[1][3];
    float b_cc = m[2][0]*ir + m[2][1]*ig + m[2][2]*ib + m[2][3];

    uint16_t r_final = static_cast<uint16_t>(std::max(0.f, std::min(65535.f, r_cc * 2.5f)));
    uint16_t g_final = static_cast<uint16_t>(std::max(0.f, std::min(65535.f, g_cc * 2.5f)));
    uint16_t b_final = static_cast<uint16_t>(std::max(0.f, std::min(65535.f, b_cc * 2.5f)));

    output.for_each_element([&](int ix, int iy, int ic) {
        if (ic == 0) ASSERT_NEAR(output(ix, iy, ic), r_final, 5);
        if (ic == 1) ASSERT_NEAR(output(ix, iy, ic), g_final, 5);
        if (ic == 2) ASSERT_NEAR(output(ix, iy, ic), b_final, 5);
    });
}

void test_blue_channel_clipping() {
    std::cout << "--- Running test: test_blue_channel_clipping ---\n";
    Halide::Var x, y, c;
    // Use a carefully chosen raw value and exposure that will clip in the old order but not the new one.
    auto raw_buffer = make_constant_buffer<uint16_t>({4, 4}, 2695);
    Halide::Func raw_func = buffer_to_func(raw_buffer, "raw_func_clip");
    Halide::Func raw_func_f("raw_func_f_clip");
    raw_func_f(x, y) = Halide::cast<float>(raw_func(x,y));
    
    float _matrix_3200[][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f}, {-0.3576f, 1.0615f, 1.5949f, -37.1158f}, {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};
    float _matrix_7000[][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f}, {-0.3826f, 1.5906f, -0.2080f, -25.4311f}, {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};
    Halide::Buffer<float> matrix_buf_3200(4, 3), matrix_buf_7000(4, 3);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) {
        matrix_buf_3200(j, i) = _matrix_3200[i][j];
        matrix_buf_7000(j, i) = _matrix_7000[i][j];
    }
    Halide::Func mat3200_f = buffer_to_func(matrix_buf_3200, "mat3200_clip");
    Halide::Func mat7000_f = buffer_to_func(matrix_buf_7000, "mat7000_clip");

    Halide::Func deinterleaved = pipeline_deinterleave(raw_func_f, x, y, c);
    DemosaicBuilder demosaic_b(deinterleaved, x, y, c, 0, 2, 2);
    Halide::Func demosaiced = demosaic_b.output;

    // Simulate OLD, incorrect order: exposure -> color_correct (but with float)
    // Use 7000K to select matrix_3200, which has the highest blue multiplier.
    Halide::Func exposed_first = pipeline_exposure(demosaiced, 5.0f, x, y, c);
    Halide::Func cc_second = pipeline_color_correct(exposed_first, mat3200_f, mat7000_f, 7000.f, 0.f, x, y, c, Halide::get_host_target(), false);
    Halide::Func to_u16_old("to_u16_old");
    to_u16_old(x,y,c) = Halide::cast<uint16_t>(Halide::clamp(cc_second(x,y,c), 0.f, 65535.f));
    Halide::Buffer<uint16_t> old_order_output = to_u16_old.realize({2, 2, 3});
    
    ASSERT_TRUE(old_order_output(0,0,0) < 65535); // Red should not clip
    ASSERT_TRUE(old_order_output(0,0,1) < 65535); // Green should not clip
    ASSERT_EQUAL(old_order_output(0,0,2), 65535); // Blue SHOULD clip

    // Simulate NEW, correct order: color_correct -> exposure
    Halide::Func cc_first = pipeline_color_correct(demosaiced, mat3200_f, mat7000_f, 7000.f, 0.f, x, y, c, Halide::get_host_target(), false);
    Halide::Func exposed_second = pipeline_exposure(cc_first, 5.0f, x, y, c);
    Halide::Func to_u16_new("to_u16_new");
    to_u16_new(x,y,c) = Halide::cast<uint16_t>(Halide::clamp(exposed_second(x,y,c), 0.f, 65535.f));
    Halide::Buffer<uint16_t> new_order_output = to_u16_new.realize({2, 2, 3});
    
    ASSERT_TRUE(new_order_output(0,0,0) < 65535); // Red should not clip
    ASSERT_TRUE(new_order_output(0,0,1) < 65535); // Green should not clip
    ASSERT_TRUE(new_order_output(0,0,2) < 65535); // Blue should NOT clip
}
