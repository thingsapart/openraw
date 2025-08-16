// This define MUST be the very first thing in the file, before any includes.
// This ensures that when imgui.h is included (via curves_editor.h), the math operators are enabled.
#define IMGUI_DEFINE_MATH_OPERATORS

#include "curves_editor.h"
#include "tone_curve_utils.h"
#include "imgui_internal.h" // For ImLengthSqr and other internal helpers
#include <algorithm> // for std::sort
#include <map>

// --- Persistent State Management ---
namespace {
    struct CurvesEditorState {
        int selected_point_index = -1;
        bool is_dragging = false;
    };
    // A static map to hold the state for each unique CurvesEditor instance.
    static std::map<ImGuiID, CurvesEditorState> s_EditorStates;

    // Maps a curve point from its logical coordinate space to the screen's pixel space.
    // This is the corrected version that handles endpoints properly.
    ImVec2 point_to_screen_generic(const Point& p, const ImVec2& canvas_pos, const ImVec2& canvas_size, float y_min, float y_max) {
        float y_range = y_max - y_min;
        float y_norm = (y_range > 1e-6f) ? (p.y - y_min) / y_range : 0.5f;
        return ImVec2(canvas_pos.x + p.x * (canvas_size.x - 1.0f),
                      canvas_pos.y + (1.0f - y_norm) * (canvas_size.y - 1.0f));
    }

    // Maps a screen pixel coordinate back to a logical curve point coordinate.
    Point screen_to_point_generic(const ImVec2& s, const ImVec2& canvas_pos, const ImVec2& canvas_size, float y_min, float y_max) {
        float y_norm = 1.0f - (s.y - canvas_pos.y) / (canvas_size.y - 1.0f);
        return Point{
            std::max(0.0f, std::min(1.0f, (s.x - canvas_pos.x) / (canvas_size.x - 1.0f))),
            y_norm * (y_max - y_min) + y_min
        };
    }
}

bool CurvesEditor::render(const char* id_str, std::vector<Point>& points, const ImVec2& size,
                          float default_y, bool is_additive, bool is_identity, float y_min, float y_max)
{
    bool changed = false;
    ImGui::PushID(id_str);
    ImGuiID id = ImGui::GetID("##canvas");
    
    // Get persistent state for this widget instance.
    CurvesEditorState& state = s_EditorStates[id];

    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const ImU32 color = IM_COL32(224, 224, 224, 255);
    const float hover_radius_sq = 5.0f * 5.0f;

    ImGui::InvisibleButton("canvas", size);
    bool canvas_hovered = ImGui::IsItemHovered();
    
    draw_readonly_spline(draw_list, points, default_y, is_additive, is_identity, canvas_pos, size, color, y_min, y_max);

    // --- Interaction Logic ---
    int hovered_point_index = -1;
    if (canvas_hovered && !state.is_dragging) {
        for (size_t i = 0; i < points.size(); ++i) {
            Point p_visual = points[i];
            if (is_identity) { p_visual.y = std::max(0.0f, std::min(1.0f, p_visual.y)); }
            ImVec2 pt_screen_visual = point_to_screen_generic(p_visual, canvas_pos, size, y_min, y_max);
            if (ImLengthSqr(io.MousePos - pt_screen_visual) < hover_radius_sq) {
                hovered_point_index = i;
                break;
            }
        }
    }

    if (state.is_dragging) {
        if (io.MouseDown[0]) {
            Point new_pos = screen_to_point_generic(io.MousePos, canvas_pos, size, y_min, y_max);
            
            // For tone curves, only interior points are clamped vertically. For other curves, all points are clamped.
            bool should_clamp_y = !is_identity || (state.selected_point_index > 0 && state.selected_point_index < points.size() - 1);
            if (should_clamp_y) {
                points[state.selected_point_index].y = std::max(y_min, std::min(y_max, new_pos.y));
            } else {
                points[state.selected_point_index].y = new_pos.y;
            }

            if (state.selected_point_index > 0 && state.selected_point_index < points.size() - 1) {
                points[state.selected_point_index].x = std::max(points[state.selected_point_index - 1].x, std::min(points[state.selected_point_index + 1].x, new_pos.x));
            }
            changed = true;
        } else {
            state.is_dragging = false;
            state.selected_point_index = -1;
        }
    } else if (canvas_hovered) {
        if (hovered_point_index != -1) {
            if (ImGui::IsMouseDown(0)) { // Use IsMouseDown to start drag immediately
                state.is_dragging = true;
                state.selected_point_index = hovered_point_index;
            } else if (io.MouseClicked[1]) {
                if (!is_identity || (hovered_point_index > 0 && hovered_point_index < points.size() - 1)) {
                    points.erase(points.begin() + hovered_point_index);
                    changed = true;
                }
            }
        } else if (ImGui::IsMouseDown(0)) { // Add point and immediately start dragging it
            Point new_point = screen_to_point_generic(io.MousePos, canvas_pos, size, y_min, y_max);
            points.push_back(new_point);
            std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
            
            // Find the index of the newly added point
            int new_index = -1;
            for(size_t i=0; i < points.size(); ++i) {
                if (std::abs(points[i].x - new_point.x) < 1e-6f && std::abs(points[i].y - new_point.y) < 1e-6f) {
                    new_index = i;
                    break;
                }
            }

            if (new_index != -1) {
                state.is_dragging = true;
                state.selected_point_index = new_index;
            }
            changed = true;
        }
    }
    
    // --- Drawing Logic for Handles and Visual Aids ---
    for (size_t i = 0; i < points.size(); ++i) {
        Point p_true = points[i];
        ImVec2 pt_screen_true = point_to_screen_generic(p_true, canvas_pos, size, y_min, y_max);
        
        Point p_visual = p_true;
        if (is_identity) { p_visual.y = std::max(0.0f, std::min(1.0f, p_visual.y)); }
        ImVec2 pt_screen_visual = point_to_screen_generic(p_visual, canvas_pos, size, y_min, y_max);

        if (is_identity && pt_screen_true.y != pt_screen_visual.y) {
            draw_list->AddLine(pt_screen_visual, pt_screen_true, IM_COL32(255, 255, 255, 60), 1.0f);
        }

        ImU32 handle_color = (i == hovered_point_index || i == state.selected_point_index) ? IM_COL32(255, 255, 255, 255) : color;
        float handle_radius = (i == hovered_point_index || i == state.selected_point_index) ? 6.0f : 4.0f;
        draw_list->AddCircleFilled(pt_screen_visual, handle_radius, handle_color);
    }
    
    ImGui::PopID();
    return changed;
}


void CurvesEditor::draw_readonly_spline(ImDrawList* draw_list, const std::vector<Point>& points,
                                        float default_y, bool is_additive, bool is_identity,
                                        const ImVec2& canvas_pos, const ImVec2& canvas_size, ImU32 color,
                                        float y_min, float y_max)
{
    ToneCurveUtils::Spline spline(points, default_y, is_additive, is_identity);
    std::vector<ImVec2> line_points;
    line_points.reserve((int)canvas_size.x + 1);
    for (int i = 0; i <= (int)canvas_size.x; ++i) {
        float x_norm = (float)i / (canvas_size.x -1.0f);
        if (i == (int)canvas_size.x) x_norm = 1.0f; // Ensure last point is exactly 1.0
        float y_val = spline.evaluate(x_norm);
        float y_draw = y_val;
        // Apply visual flattening only for the identity (tone) curve
        if (is_identity) {
            y_draw = std::max(0.0f, std::min(1.0f, y_val));
        }
        line_points.push_back(point_to_screen_generic({x_norm, y_draw}, canvas_pos, canvas_size, y_min, y_max));
    }
    draw_list->AddPolyline(line_points.data(), line_points.size(), color, ImDrawFlags_None, 1.5f);
}
