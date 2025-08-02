#include "test_harness.h"
#include "stage_deinterleave.h"
#include "stage_demosaic.h"

void test_hot_pixel_propagation() {
    std::cout << "--- Running test: test_hot_pixel_propagation ---\n";
    Halide::Var x, y, c;

    // Create a 4x4 RAW buffer of all zeros.
    Halide::Buffer<uint16_t> raw_buffer(4, 4);
    raw_buffer.fill(0);

    // Set a single blue pixel to a hot value. A blue pixel is at an even x, odd y.
    raw_buffer(2, 1) = 65535;

    Halide::Func raw_func = buffer_to_func(raw_buffer, "hot_pixel_input");
    // Convert to float before deinterleaving
    Halide::Func raw_func_f("raw_func_f");
    raw_func_f(x, y) = Halide::cast<float>(raw_func(x,y));

    Halide::Func deinterleaved = pipeline_deinterleave(raw_func_f, x, y, c);
    
    // Pass the deinterleaved func and its dimensions (4x4 raw -> 2x2 deinterleaved)
    // to the DemosaicBuilder. Algorithm is 'simple' (0).
    DemosaicBuilder builder(deinterleaved, x, y, c, 0, 2, 2);

    Halide::Buffer<float> output = builder.output.realize({4, 4, 3});

    int non_zero_pixels = 0;
    output.for_each_value([&](float& val) {
        if (val > 0) {
            non_zero_pixels++;
        }
    });

    // The single hot pixel should be smeared across multiple output pixels by demosaic.
    ASSERT_TRUE(non_zero_pixels > 1);
}
