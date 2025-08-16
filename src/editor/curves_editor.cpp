#include "curves_editor.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <algorithm>
#include <cmath>

// Convert world coordinates (0-1) to screen coordinates.
ImVec2 CurvesEditor::world_to_screen(const Point& p, const ImVec2& canvas_pos, const ImVec2& canvas_size) {
    return ImVec2(
        canvas_pos.x + p.x * canvas_size.x,
        canvas_pos.y + (1.0f - p.y) * canvas_size.y // Y is inverted
    );
}

// Convert screen coordinates to world coordinates (0-1).
Point CurvesEditor::screen_to_world(const ImVec2& p, const ImVec2& canvas_pos, const ImVec2& canvas_size) {
    return Point{
        (p.x - canvas_pos.x) / canvas_size.x,
        1.0f - (p.y - canvas_pos.y) / canvas_size.y // Y is inverted
    };
}

void CurvesEditor::draw_readonly_curve(ImDrawList* draw_list, const Halide::Runtime::Buffer<uint16_t, 2>& lut, int channel_idx, const ImVec2& canvas_pos, const ImVec2& canvas_size, ImU32 color) {
    if (lut.data() == nullptr || lut.dimensions() < 2 || lut.dim(1).extent() <= channel_idx) return;

    std::vector<ImVec2> points;
    const int lut_size = lut.width();
    const int num_segments = std::min((int)canvas_size.x, 256);
    points.reserve(num_segments + 1);
    
    for (int i = 0; i <= num_segments; ++i) {
        float norm_x = (float)i / num_segments;
        int lut_idx = (int)(norm_x * (lut_size - 1));
        // Clamp the Y value for visualization, even though the underlying data might be out of range.
        float norm_y = std::max(0.0f, std::min(1.0f, (float)lut(lut_idx, channel_idx) / 65535.0f));
        points.push_back(world_to_screen({norm_x, norm_y}, canvas_pos, canvas_size));
    }
    draw_list->AddPolyline(points.data(), points.size(), color, ImDrawFlags_None, 1.5f);
}


bool CurvesEditor::render_multi_channel(
        std::vector<Point>& active_points,
        const Halide::Runtime::Buffer<uint16_t, 2>& lut,
        int active_channel_idx,
        const ImVec2& canvas_pos, const ImVec2& canvas_size,
        ImU32 active_color,
        bool is_hovered, bool is_active, const ImGuiIO& io) {

    bool changed = false;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    
    const float handle_radius = 6.0f;
    const float grab_radius = 16.0f; 

    // --- Interaction Logic ---
    Point mouse_world_pos = screen_to_world(io.MousePos, canvas_pos, canvas_size);
        
    if (active_point_idx == -1) {
        hovered_point_idx = -1;
        if (is_hovered) {
            for (size_t i = 0; i < active_points.size(); ++i) {
                // Clamp the point's Y for hit-testing so it can be grabbed even if dragged off-screen.
                Point clamped_point = active_points[i];
                clamped_point.y = std::max(0.0f, std::min(1.0f, clamped_point.y));
                ImVec2 point_screen_pos = world_to_screen(clamped_point, canvas_pos, canvas_size);

                if (ImLengthSqr(io.MousePos - point_screen_pos) < grab_radius * grab_radius) {
                    hovered_point_idx = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered_point_idx != -1) {
            active_point_idx = hovered_point_idx;
        } else {
            active_points.push_back({
                std::max(0.0f, std::min(1.0f, mouse_world_pos.x)),
                std::max(0.0f, std::min(1.0f, mouse_world_pos.y))
            });
            std::sort(active_points.begin(), active_points.end(), [](const Point& a, const Point& b){ return a.x < b.x; });
            for (size_t i = 0; i < active_points.size(); ++i) {
                 if (fabsf(active_points[i].x - mouse_world_pos.x) < 1e-6f) {
                    active_point_idx = i;
                    break;
                }
            }
            changed = true;
        }
    }
    
    if (is_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && active_point_idx != -1) {
        bool is_endpoint = (active_point_idx == 0 || active_point_idx == (int)active_points.size() - 1);
        if (is_endpoint) {
            // Endpoints can be moved freely on the Y-axis to clip blacks/whites.
            active_points[active_point_idx].y = mouse_world_pos.y;
        } else {
            // Interior points are clamped to the visible area.
            active_points[active_point_idx].x = std::max(0.0f, std::min(1.0f, mouse_world_pos.x));
            active_points[active_point_idx].y = std::max(0.0f, std::min(1.0f, mouse_world_pos.y));
            if (active_point_idx > 0) active_points[active_point_idx].x = std::max(active_points[active_point_idx].x, active_points[active_point_idx - 1].x);
            if (active_point_idx < (int)active_points.size() - 1) active_points[active_point_idx].x = std::min(active_points[active_point_idx].x, active_points[active_point_idx + 1].x);
        }
        changed = true;
    }
    
    if (is_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered_point_idx != -1) {
        if (hovered_point_idx > 0 && hovered_point_idx < (int)active_points.size() - 1) {
            active_points.erase(active_points.begin() + hovered_point_idx);
            hovered_point_idx = -1;
            active_point_idx = -1;
            changed = true;
        }
    }
    
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        active_point_idx = -1;
        if (changed) {
            std::sort(active_points.begin(), active_points.end(), [](const Point& a, const Point& b){ return a.x < b.x; });
        }
    }

    // --- Drawing Logic ---
    draw_list->AddLine(
        ImVec2(canvas_pos.x, canvas_pos.y + canvas_size.y), 
        ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y), 
        IM_COL32(100, 100, 100, 100));

    draw_readonly_curve(draw_list, lut, active_channel_idx, canvas_pos, canvas_size, active_color);

    for (size_t i = 0; i < active_points.size(); ++i) {
        // Clamp the point's Y position for drawing, so handles don't go off-screen.
        Point clamped_point = active_points[i];
        clamped_point.y = std::max(0.0f, std::min(1.0f, clamped_point.y));
        ImVec2 p = world_to_screen(clamped_point, canvas_pos, canvas_size);

        ImU32 point_color = (i == (size_t)active_point_idx) ? IM_COL32(255, 200, 0, 255) : 
                            (i == (size_t)hovered_point_idx) ? IM_COL32(255, 255, 255, 255) : 
                                                         active_color;
        
        draw_list->AddCircleFilled(p, handle_radius, point_color);
        if (i > 0 && i < active_points.size() - 1) {
             draw_list->AddCircle(p, handle_radius, IM_COL32(0, 0, 0, 200), 0, 1.0f);
        }
    }
    
    return changed;
}
