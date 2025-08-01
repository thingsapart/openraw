#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <limits>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace Halide {
namespace Tools {

#if !(defined(__EMSCRIPTEN__) && defined(HALIDE_BENCHMARK_USE_EMSCRIPTEN_GET_NOW))

// Prefer high_resolution_clock, but only if it's steady...
template<bool HighResIsSteady = std::chrono::high_resolution_clock::is_steady>
struct SteadyClock {
    using type = std::chrono::high_resolution_clock;
};

// ...otherwise use steady_clock.
template<>
struct SteadyClock<false> {
    using type = std::chrono::steady_clock;
};

inline SteadyClock<>::type::time_point benchmark_now() {
    return SteadyClock<>::type::now();
}

inline double benchmark_duration_seconds(
    SteadyClock<>::type::time_point start,
    SteadyClock<>::type::time_point end) {
    return std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
}

#else  // __EMSCRIPTEN__

inline double benchmark_now() {
    return emscripten_get_now();
}

inline double benchmark_duration_seconds(double start, double end) {
    return std::max((end - start) / 1000.0, 1e-9);
}

#endif

inline double benchmark(uint64_t samples, uint64_t iterations, const std::function<void()> &op) {
    double best = std::numeric_limits<double>::infinity();
    for (uint64_t i = 0; i < samples; i++) {
        auto start = benchmark_now();
        for (uint64_t j = 0; j < iterations; j++) {
            op();
        }
        auto end = benchmark_now();
        double elapsed_seconds = benchmark_duration_seconds(start, end);
        best = std::min(best, elapsed_seconds);
    }
    return best / iterations;
}

constexpr uint64_t kBenchmarkMaxIterations = 1000000000;

struct BenchmarkConfig {
    double min_time{0.1};
    double max_time{0.1 * 4};
    uint64_t max_iters_per_sample{1000000};
    double accuracy{0.03};
};

struct BenchmarkResult {
    double wall_time;
    uint64_t samples;
    uint64_t iterations;
    double accuracy;

    operator double() const {
        return wall_time;
    }
};

inline BenchmarkResult benchmark(const std::function<void()> &op, const BenchmarkConfig &config = {}) {
    BenchmarkResult result{0, 0, 0};

    const double min_time = std::max(10 * 1e-6, config.min_time);
    const double max_time = std::max(config.min_time, config.max_time);
    const double accuracy = 1.0 + std::min(std::max(0.001, config.accuracy), 0.1);

    constexpr int kMinSamples = 3;
    double times[kMinSamples + 1] = {0};

    double total_time = 0;
    uint64_t iters_per_sample = 1;
    for (;;) {
        result.samples = 0;
        result.iterations = 0;
        total_time = 0;
        for (int i = 0; i < kMinSamples; i++) {
            times[i] = benchmark(1, iters_per_sample, op);
            result.samples++;
            result.iterations += iters_per_sample;
            total_time += times[i] * iters_per_sample;
        }
        std::sort(times, times + kMinSamples);

        const double kTimeEpsilon = 1e-9;
        if (times[0] < kTimeEpsilon) {
            iters_per_sample *= 2;
        } else {
            const double time_factor = std::max(times[0] * kMinSamples, kTimeEpsilon);
            if (time_factor * iters_per_sample >= min_time) {
                break;
            }
            const double next_iters = std::max(min_time / time_factor,
                                               iters_per_sample * 2.0);
            iters_per_sample = (uint64_t)(next_iters + 0.5);
        }

        if (iters_per_sample >= config.max_iters_per_sample) {
            iters_per_sample = config.max_iters_per_sample;
            break;
        }
    }

    while ((times[0] * accuracy < times[kMinSamples - 1] || total_time < min_time) &&
           total_time < max_time) {
        times[kMinSamples] = benchmark(1, iters_per_sample, op);
        result.samples++;
        result.iterations += iters_per_sample;
        total_time += times[kMinSamples] * iters_per_sample;
        std::sort(times, times + kMinSamples + 1);
    }
    result.wall_time = times[0];
    result.accuracy = (times[kMinSamples - 1] / times[0]) - 1.0;

    return result;
}

}  // namespace Tools
}  // namespace Halide

#endif
