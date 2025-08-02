#include "test_harness.h"
#include "stage_exposure.h"

void test_exposure() {
    std::cout << "--- Running test: test_exposure ---\n";
    Halide::Var x, y, c;

    auto input_buffer = make_constant_buffer<uint16_t>({10, 10, 3}, 1000);
    Halide::Func input_func = buffer_to_func(input_buffer, "input_func_exposure");

    Halide::Func exposed = pipeline_exposure(input_func, 4.5f, x, y, c);
    
    Halide::Buffer<uint16_t> output = exposed.realize({10, 10, 3});
    
    output.for_each_value([](uint16_t& val) {
        ASSERT_EQUAL(val, 4500);
    });
}
