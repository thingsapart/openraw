#ifndef SIMPLE_TIMER_H
#define SIMPLE_TIMER_H

#include <chrono>
#include <string>
#include <iostream>
#include <iomanip>

// A simple RAII timer class.
// When an object of this class is created, it records the start time.
// When it goes out of scope, its destructor is called, which records the end
// time, calculates the duration, and prints a formatted message.
class SimpleTimer {
public:
    explicit SimpleTimer(const std::string& name, bool enabled = true)
        : name_(name), enabled_(enabled), start_time_(std::chrono::high_resolution_clock::now()) {}

    ~SimpleTimer() {
        if (enabled_) {
            auto end_time = std::chrono::high_resolution_clock::now();
            auto start = std::chrono::time_point_cast<std::chrono::microseconds>(start_time_).time_since_epoch().count();
            auto end = std::chrono::time_point_cast<std::chrono::microseconds>(end_time).time_since_epoch().count();

            auto duration_us = end - start;
            double duration_ms = duration_us / 1000.0;

            std::cout << std::fixed << std::setprecision(2)
                      << "Host-side time for '" << name_ << "': "
                      << duration_ms << " ms" << std::endl;
        }
    }

private:
    std::string name_;
    bool enabled_;
    std::chrono::high_resolution_clock::time_point start_time_;
};

#endif // SIMPLE_TIMER_H
