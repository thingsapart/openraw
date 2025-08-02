#include "test_harness.h"
#include "stage_saturation.h"

void test_saturation_out_of_gamut() {
    std::cout << "--- Running test: test_saturation_out_of_gamut ---\n";
    Halide::Var x, y, c;
    
    // Case 1: Pure saturated blue. Should not corrupt R and G channels.
    {
        Halide::Buffer<uint16_t> input_buffer(1, 1, 3);
        input_buffer(0,0,0) = 0;
        input_buffer(0,0,1) = 0;
        input_buffer(0,0,2) = 65535;
        Halide::Func input_func = buffer_to_func(input_buffer, "sat_blue_input");

        // Algorithm 1 is L*a*b*
        Halide::Func saturated = pipeline_saturation(input_func, 1.5f, 1, x, y, c);
        Halide::Buffer<uint16_t> output = saturated.realize({1, 1, 3});

        ASSERT_NEAR(output(0,0,0), 0, 1); // R should stay near 0
        ASSERT_NEAR(output(0,0,1), 0, 1); // G should stay near 0
        ASSERT_TRUE(output(0,0,2) > 65000); // B should remain high
    }

    // Case 2: Pure saturated green.
    {
        Halide::Buffer<uint16_t> input_buffer(1, 1, 3);
        input_buffer(0,0,0) = 0;
        input_buffer(0,0,1) = 65535;
        input_buffer(0,0,2) = 0;
        Halide::Func input_func = buffer_to_func(input_buffer, "sat_green_input");

        Halide::Func saturated = pipeline_saturation(input_func, 1.5f, 1, x, y, c);
        Halide::Buffer<uint16_t> output = saturated.realize({1, 1, 3});

        ASSERT_NEAR(output(0,0,0), 0, 1); // R should stay near 0
        ASSERT_TRUE(output(0,0,1) > 65000); // G should remain high
        ASSERT_NEAR(output(0,0,2), 0, 1); // B should stay near 0
    }

    // Case 3: Grayscale. Saturation should have no effect.
    {
        auto input_buffer = make_constant_buffer<uint16_t>({2, 2, 3}, 32768);
        Halide::Func input_func = buffer_to_func(input_buffer, "sat_gray_input");
        
        Halide::Func saturated = pipeline_saturation(input_func, 1.5f, 1, x, y, c);
        Halide::Buffer<uint16_t> output = saturated.realize({2, 2, 3});

        output.for_each_value([](uint16_t& val) {
            ASSERT_NEAR(val, 32768, 2); // Allow for small rounding differences
        });
    }
}

void test_saturation_on_dark_colors() {
    std::cout << "--- Running test: test_saturation_on_dark_colors ---\n";
    Halide::Var x, y, c;
    
    // Input is a dark, slightly blueish color, simulating a shadow area.
    Halide::Buffer<uint16_t> input_buffer(1, 1, 3);
    input_buffer(0,0,0) = 50;  // R
    input_buffer(0,0,1) = 50;  // G
    input_buffer(0,0,2) = 100; // B
    Halide::Func input_func = buffer_to_func(input_buffer, "sat_dark_blue_input");

    // Use a high saturation value to amplify any potential issues.
    Halide::Func saturated = pipeline_saturation(input_func, 2.0f, 1, x, y, c); // L*a*b* algo
    Halide::Buffer<uint16_t> output = saturated.realize({1, 1, 3});

    // The output should be more saturated, but hue should be preserved and values shouldn't explode.
    // Blue should still be the dominant channel.
    ASSERT_TRUE(output(0,0,2) > output(0,0,0));
    ASSERT_TRUE(output(0,0,2) > output(0,0,1));

    // Most importantly, check that other channels don't get spurious huge values.
    // If R or G becomes large, it indicates a hue shift. If any value clips, it's a problem.
    ASSERT_TRUE(output(0,0,0) < 200);
    ASSERT_TRUE(output(0,0,1) < 200);
    ASSERT_TRUE(output(0,0,2) < 400); // Blue will be amplified, but shouldn't explode.
}
