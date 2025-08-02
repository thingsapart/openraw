#include "test_harness.h"
#include "tone_curve_utils.h"

void test_tone_curve_lut_generation() {
    std::cout << "--- Running test: test_tone_curve_lut_generation ---\n";
    ProcessConfig cfg;
    cfg.gamma = 1.0f;
    cfg.contrast = 0.0f;
    cfg.exposure = 1.0f;
    cfg.black_point = 0.0f;
    
    ToneCurveUtils util(cfg);
    auto lut = util.get_lut_for_halide();

    ASSERT_EQUAL(lut.dimensions(), 2);
    ASSERT_EQUAL(lut.width(), 65536);
    ASSERT_EQUAL(lut.height(), 3);
    ASSERT_EQUAL(lut(0, 0), 0);
    ASSERT_EQUAL(lut(1000, 1), 1000);
    ASSERT_EQUAL(lut(32768, 2), 32768);
    ASSERT_EQUAL(lut(65535, 0), 65535);
}

void test_perceptual_curve_mapping() {
    std::cout << "--- Running test: test_perceptual_curve_mapping ---\n";
    ProcessConfig cfg;
    cfg.curve_points_str = "0.25:0.15,0.75:0.85";
    ToneCurveUtils util(cfg);
    auto lut = util.get_lut_for_halide();

    // A linear mid-gray (18% reflectance) is ~12-13% of the numeric range.
    // Let's test the value at index 8192 (12.5% of 65536).
    const int test_idx = 8192;
    float x_norm = (float)test_idx / 65535.0f; // ~0.125

    // Manually calculate the expected hermite spline output
    float x0=0.0f, y0=0.0f, x1=0.25f, y1=0.15f, x2=0.75f, y2=0.85f, x3=1.0f, y3=1.0f;
    float m0 = (y1-y0)/(x1-x0); // 0.6
    float m2_slope_next = (y3-y2)/(x3-x2);
    float m2_slope_prev = (y2-y1)/(x2-x1);
    float m2 = (m2_slope_prev + m2_slope_next)/2.0f;
    float m1_slope_next = (y2-y1)/(x2-x1);
    float m1_slope_prev = (y1-y0)/(x1-x0);
    float m1 = (m1_slope_prev + m1_slope_next)/2.0f; // (0.6 + 1.4)/2 = 1.0
    float h = x1 - x0;
    float t = (x_norm - x0) / h;
    float t2 = t*t, t3 = t2*t;
    float h00=2*t3-3*t2+1, h10=t3-2*t2+t, h01=-2*t3+3*t2, h11=t3-t2;
    float mapped_val = h00*y0 + h10*h*m0 + h01*y1 + h11*h*m1;
    uint16_t expected_lut_val = static_cast<uint16_t>(mapped_val * 65535.0f + 0.5f);

    // We expect a dark value because 0.125 is early in the curve.
    ASSERT_EQUAL(lut(test_idx, 0), expected_lut_val);
    ASSERT_TRUE(lut(test_idx, 0) < 5000);
}

void test_default_s_curve_range() {
    std::cout << "--- Running test: test_default_s_curve_range ---\n";
    
    ProcessConfig cfg;
    // Use default values: exposure=1.0, black_point=25, white_point=4095
    
    ToneCurveUtils util(cfg);
    auto lut = util.get_lut_for_halide();

    // This test verifies the fix for the "too dark" image problem.
    // It asserts that the black and white points from the config are
    // correctly used to generate the default S-curve.

    // The value at the specified white point (after exposure) should map to max output.
    int white_level = static_cast<int>(cfg.white_point * cfg.exposure);
    ASSERT_NEAR(lut(white_level, 1), 65535, 2);
    
    // The value at the black point should map to 0.
    int black_level = static_cast<int>(cfg.black_point * cfg.exposure);
    ASSERT_NEAR(lut(black_level, 1), 0, 2);
    
    // A value halfway between black and white should map to a mid-gray.
    int mid_level = (white_level + black_level) / 2;
    uint16_t mid_out = lut(mid_level, 1);
    // The exact value depends on gamma and contrast, but it should be far from the extremes.
    ASSERT_TRUE(mid_out > 10000 && mid_out < 55000);
}
