#ifndef APP_STATE_H
#define APP_STATE_H

// This define enables the + and - operators for ImVec2, which is very useful.
// It must be defined before any imgui headers are included.
#define IMGUI_DEFINE_MATH_OPERATORS

#include "process_options.h"
#include "HalideBuffer.h"
#include "imgui.h" // Include the main Dear ImGui header to define ImVec2

#include <chrono>
#include <cstdint>
#include <vector>

// UI State management struct
// This is the central data structure passed to UI rendering functions.
struct AppState {
    // Static configuration for debouncing
    static constexpr std::chrono::milliseconds DEBOUNCE_DURATION{300};

    ProcessConfig params;

    // Halide Buffers
    Halide::Runtime::Buffer<uint16_t> input_image;
    Halide::Runtime::Buffer<uint8_t> main_output_planar;
    Halide::Runtime::Buffer<uint8_t> thumb_output_planar;

    // CPU-side copy of interleaved data for OpenGL
    std::vector<uint8_t> main_output_interleaved;
    std::vector<uint8_t> thumb_output_interleaved;

    // OpenGL Textures
    uint32_t main_texture_id = 0;
    uint32_t thumb_texture_id = 0;

    // --- New Viewport "Camera" Model ---
    // These are the "source of truth" for the view.
    ImVec2 view_center_norm{0.5f, 0.5f}; // The point in the source image at the center of the view. (0-1)
    float view_scale = 1.0f;              // Fraction of the source image width visible in the view. 1.0 = fit-width.

    // --- UI State (updated each frame) ---
    ImVec2 main_view_size{1, 1};
    ImVec2 thumb_view_size{1, 1};

    // --- Debounce State ---
    std::chrono::steady_clock::time_point next_render_time = std::chrono::steady_clock::time_point::max();
    bool ui_ready = false;
};

#endif // APP_STATE_H
