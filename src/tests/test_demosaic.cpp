#include "test_harness.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h"

void test_demosaic_behavior() {
    std::cout << "--- Running test: test_demosaic_behavior ---\n";
    Halide::Var x, y, c;

    // Case 1: Flat dark area. Should not produce negative values.
    {
        auto raw_buffer = make_constant_buffer<uint16_t>({8, 8}, 50);
        Halide::Func raw_func = buffer_to_func(raw_buffer, "demosaic_flat_input");
        Halide::Func deinterleaved = pipeline_deinterleave(raw_func, x, y, c);
        
        // The DemosaicBuilder now handles boundary conditions internally.
        // We pass the deinterleaved func directly, along with its dimensions.
        // The algorithm parameter is 0 for 'simple'.
        DemosaicBuilder builder(deinterleaved, x, y, c, 0, 4, 4);
        
        Halide::Buffer<float> output = builder.output.realize({8, 8, 3});

        // The output should be very close to the input value and non-negative.
        output.for_each_value([&](float& val) {
            ASSERT_NEAR(val, 50.f, 2.f); // Allow for small rounding differences
        });
    }

    // Case 2: Adversarial input designed to create a negative green correction.
    {
        // G_r is dark, its R/B neighbors are bright. The average of R/B neighbors will be > G_r.
        Halide::Buffer<uint16_t> raw_buffer(6, 6);
        raw_buffer.fill(1000); // Background
        // Create a 2x2 pattern of interest in the middle
        raw_buffer(2, 2) = 500;  // G_r (dark)
        raw_buffer(3, 2) = 2000; // R   (bright)
        raw_buffer(2, 3) = 2000; // B   (bright)
        raw_buffer(3, 3) = 500;  // G_b (dark)

        Halide::Func raw_func = buffer_to_func(raw_buffer, "demosaic_adversarial_input");
        Halide::Func deinterleaved = pipeline_deinterleave(raw_func, x, y, c);
        
        DemosaicBuilder builder(deinterleaved, x, y, c, 0, 3, 3);

        Halide::Buffer<float> output = builder.output.realize({6, 6, 3});

        // With Float32, the green correction can go negative without wrapping around.
        // The final interpolated color should still be positive due to averaging.
        // This test now verifies that the output is not zero and not a huge wrapped-around value.
        float red_at_dark_green = output(2, 2, 0);
        ASSERT_TRUE(red_at_dark_green > 0.f);
        ASSERT_TRUE(red_at_dark_green < 3000.f); // Sanity check for no huge values
    }
}
