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
#include <memory> // For std::unique_ptr

// To enable Lensfun support, compile with -DUSE_LENSFUN and link against liblensfun.
#ifdef USE_LENSFUN
#include "lensfun/lensfun.h"

// --- C++ RAII Wrappers for Lensfun C API ---

// Custom deleter for the lfDatabase object.
struct LfDatabaseDeleter {
    void operator()(lfDatabase* db) const {
        if (db) {
            lf_db_destroy(db);
        }
    }
};
using LfDatabasePtr = std::unique_ptr<lfDatabase, LfDatabaseDeleter>;

// Custom deleter for memory allocated by lensfun that needs lf_free().
// This is used for the arrays returned by the lf_db_find_* functions.
struct LfFreeDeleter {
    void operator()(void* ptr) const {
        if (ptr) {
            lf_free(ptr);
        }
    }
};
// A template alias to make using this deleter cleaner.
template<typename T>
using LfScopedPtr = std::unique_ptr<T, LfFreeDeleter>;

#endif


// Enum to identify which curve is currently being edited in the UI.
enum class ActiveCurveChannel {
    Luma,
    Red,
    Green,
    Blue
};

// UI State management struct
// This is the central data structure passed to UI rendering functions.
struct AppState {
    // Static configuration for debouncing
    static constexpr std::chrono::milliseconds DEBOUNCE_DURATION{200};

    ProcessConfig params;

    // Halide Buffers
    Halide::Runtime::Buffer<uint16_t> input_image;
    Halide::Runtime::Buffer<uint8_t> main_output_planar;
    Halide::Runtime::Buffer<uint8_t> thumb_output_planar;
    
    // We now maintain two separate LUTs:
    // 1. The final, combined LUT for the pipeline and histogram.
    Halide::Runtime::Buffer<uint16_t, 2> pipeline_tone_curve_lut;
    // 2. A linear-based LUT purely for the UI curve editor visualization.
    Halide::Runtime::Buffer<uint16_t, 2> ui_tone_curve_lut;


    // CPU-side copy of interleaved data for OpenGL
    std::vector<uint8_t> main_output_interleaved;
    std::vector<uint8_t> thumb_output_interleaved;

    // OpenGL Textures
    uint32_t main_texture_id = 0;
    uint32_t thumb_texture_id = 0;

    // --- Viewport State ---
    float zoom = 1.0f;                      // The logical zoom level relative to "fit-to-view"
    ImVec2 pan_offset{0, 0};                // Panning offset in screen pixels

    // --- UI State (updated each frame) ---
    ImVec2 main_view_size{1, 1};
    ImVec2 thumb_view_size{1, 1};

    // --- Histogram Data & State ---
    std::vector<float> histogram_luma; // Normalized [0,1] for plotting
    std::vector<float> histogram_r;
    std::vector<float> histogram_g;
    std::vector<float> histogram_b;
    bool show_luma_histo = true;
    bool show_r_histo = true;
    bool show_g_histo = true;
    bool show_b_histo = true;
    bool show_curve_overlay = true;
    ActiveCurveChannel active_curve_channel = ActiveCurveChannel::Luma;

    // --- Debounce State ---
    std::chrono::steady_clock::time_point next_render_time = std::chrono::steady_clock::time_point::max();
    bool ui_ready = false;

#ifdef USE_LENSFUN
    // --- Lensfun State ---
    LfDatabasePtr lensfun_db;
    // Cached lists for UI dropdowns
    std::vector<std::string> lensfun_camera_makes;
    std::vector<std::string> lensfun_camera_models; // Updated when make is selected
    std::vector<std::string> lensfun_lens_models;   // Updated when camera is selected
#endif
};

#endif // APP_STATE_H
