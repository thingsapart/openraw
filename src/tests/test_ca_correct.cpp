#include "test_harness.h"
#include "stage_ca_correct.h"

void test_ca_correction_artifacts() {
    std::cout << "--- Running test: test_ca_correction_artifacts ---\n";
    Halide::Var x, y;

    // Create a RAW image with a sharp vertical edge on the green channel.
    // This simulates high-frequency details like grass and can provoke
    // over/undershoot in the correction algorithm.
    const int W = 64, H = 64;
    Halide::Buffer<uint16_t> raw_buffer(W, H);
    raw_buffer.for_each_element([&](int ix, int iy) {
        // is_g = (x%2 + y%2) % 2 == 0
        bool is_g = ((ix % 2) + (iy % 2)) % 2 == 0;
        if (is_g) {
            raw_buffer(ix, iy) = (ix < W / 2) ? 2000 : 10000;
        } else {
            raw_buffer(ix, iy) = 2000; // R and B pixels are flat dark
        }
    });

    Halide::Func raw_func = buffer_to_func(raw_buffer, "ca_edge_input");
    CACorrectBuilder ca_builder(raw_func, x, y,
                                1.0f, // strength
                                1.0f, 16383.0f, // black/white levels (14-bit)
                                W, H,
                                Halide::get_host_target(), false);

    Halide::Buffer<uint16_t> output = ca_builder.output.realize({W, H});

    // The output values should be clamped within a reasonable range of the input [2000, 10000].
    // If the algorithm produces extreme values (e.g., >12000 or near 0), it indicates
    // an instability that could cause color clipping on real images.
    bool has_overshoot = false;
    output.for_each_value([&](uint16_t& val) {
        if (val > 11000 || val < 1500) {
            has_overshoot = true;
        }
    });
    ASSERT_TRUE(!has_overshoot);
}
