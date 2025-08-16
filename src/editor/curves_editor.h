#ifndef CURVES_EDITOR_H
#define CURVES_EDITOR_H

// This must be defined before including imgui.h to enable math operators for ImVec2
#define IMGUI_DEFINE_MATH_OPERATORS

#include "process_options.h" // For Point struct
#include "HalideBuffer.h"
#include "imgui.h"
#include <vector>

class CurvesEditor {
public:
    // Renders the multi-channel curve editor. It is now a pure drawing and logic component.
    // The parent must provide the interaction state (is_hovered, is_active).
    bool render_multi_channel(
        std::vector<Point>& active_points,
        const Halide::Runtime::Buffer<uint16_t, 2>& lut,
        int active_channel_idx,
        const ImVec2& canvas_pos, const ImVec2& canvas_size,
        ImU32 active_color,
        bool is_hovered, bool is_active, const ImGuiIO& io);

    // Public static helper for drawing read-only curves
    static void draw_readonly_curve(ImDrawList* draw_list, const Halide::Runtime::Buffer<uint16_t, 2>& lut, int channel_idx, const ImVec2& canvas_pos, const ImVec2& canvas_size, ImU32 color);

private:
    int active_point_idx = -1;
    int hovered_point_idx = -1;

    // Helper to convert world coordinates (0-1) to screen coordinates.
    static ImVec2 world_to_screen(const Point& p, const ImVec2& canvas_pos, const ImVec2& canvas_size);

    // Helper to convert screen coordinates to world coordinates (0-1).
    static Point screen_to_world(const ImVec2& p, const ImVec2& canvas_pos, const ImVec2& canvas_size);
};

#endif // CURVES_EDITOR_H
