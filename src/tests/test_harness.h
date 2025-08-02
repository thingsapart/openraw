#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <iostream>
#include <vector>
#include <string>
#include <numeric>
#include <cmath>

#include "Halide.h"

// --- Test Harness ---
// A simple assertion framework.
extern int test_failures;

#define ASSERT_TRUE(cond)                                                                \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << __FILE__ << ":" << __LINE__ << ": In function '" << __func__     \
                      << "':\n"                                                          \
                      << "  Assertion failed: " << #cond << "\n";                        \
            test_failures++;                                                             \
        }                                                                                \
    } while (0)

#define ASSERT_EQUAL(a, b)                                                               \
    do {                                                                                 \
        if ((a) != (b)) {                                                                \
            std::cerr << __FILE__ << ":" << __LINE__ << ": In function '" << __func__     \
                      << "':\n"                                                          \
                      << "  Assertion failed: " << #a << " == " << #b << "\n"            \
                      << "    Got: " << (a) << "\n"                                      \
                      << "    Expected: " << (b) << "\n";                                \
            test_failures++;                                                             \
        }                                                                                \
    } while (0)

#define ASSERT_NEAR(a, b, tolerance)                                                     \
    do {                                                                                 \
        if (std::abs((a) - (b)) > (tolerance)) {                                         \
            std::cerr << __FILE__ << ":" << __LINE__ << ": In function '" << __func__     \
                      << "':\n"                                                          \
                      << "  Assertion failed: " << #a << " ~= " << #b << "\n"            \
                      << "    Got: " << (a) << "\n"                                      \
                      << "    Expected: " << (b) << "\n"                                 \
                      << "    (tolerance: " << (tolerance) << ")\n";                     \
            test_failures++;                                                             \
        }                                                                                \
    } while (0)


// Helper to create a Halide Buffer filled with a constant value.
template <typename T>
Halide::Buffer<T> make_constant_buffer(const std::vector<int>& dims, T value) {
    Halide::Buffer<T> buffer(dims);
    buffer.for_each_value([&](T& x) { x = value; });
    return buffer;
}

// Helper to wrap a Buffer in a Func for testing pipeline stages.
template <typename T>
Halide::Func buffer_to_func(const Halide::Buffer<T>& buffer, const std::string& name) {
    Halide::ImageParam ip(buffer.type(), buffer.dimensions(), name);
    ip.set(buffer);
    
    std::vector<Halide::Var> vars;
    for (int i = 0; i < buffer.dimensions(); ++i) {
        vars.push_back(Halide::Var("v" + std::to_string(i)));
    }

    Halide::Func f(name + "_func");
    f(vars) = ip(vars);
    return f;
}

#endif // TEST_HARNESS_H
