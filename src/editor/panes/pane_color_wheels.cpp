#include "pane_color_wheels.h"
#include "../app_state.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>

namespace { // Anonymous namespace for custom widgets

// Custom Color Wheel widget implementation.
bool ColorWheel(const char* id, float radius, Point* offset, float* luma) {
    bool changed = false;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();

    // Reserve space for the wheel and its label
    ImGui::InvisibleButton(id, ImVec2(radius * 2, radius * 2));
    ImVec2 center = cursor_pos + ImVec2(radius, radius);
    
    // Interaction logic
    if (ImGui::IsItemActive()) {
        ImVec2 drag_delta = io.MousePos - center;
        float dist = sqrtf(drag_delta.x * drag_delta.x + drag_delta.y * drag_delta.y);
        
        // Clamp the handle to the radius of the wheel
        if (dist > radius) {
            drag_delta.x *= radius / dist;
            drag_delta.y *= radius / dist;
        }

        // Update the offset. The y-axis is inverted between ImGui and the color space.
        // A small change threshold avoids noisy updates.
        if (fabsf(offset->x - drag_delta.x / radius) > 1e-4f || fabsf(offset->y - -drag_delta.y / radius) > 1e-4f) {
            offset->x = drag_delta.x / radius;
            offset->y = -drag_delta.y / radius;
            changed = true;
        }
    }

    // Double-click to reset
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        offset->x = 0.0f;
        offset->y = 0.0f;
        changed = true;
    }
    
    // Drawing
    draw_list->AddCircleFilled(center, radius, IM_COL32(40, 40, 45, 255), 32);

    // Draw the colorful hue ring, mapping screen angle to CIELAB-like colors
    const float ring_width = 7.0f;
    const int num_segments = 128;
    const float inner_radius = radius - ring_width;
    if (inner_radius > 0) {
        for (int i = 0; i < num_segments; ++i) {
            const float a0 = (float)i / (float)num_segments * 2.0f * IM_PI;
            const float a1 = (float)(i + 1) / (float)num_segments * 2.0f * IM_PI;

            // Convert screen angle (CCW from right) to a hue that matches the CIELAB wheel
            // where R=0, Y=90, G=180, B=270 degrees.
            float screen_angle_deg = a0 * 180.0f / IM_PI;
            float hsv_angle_deg;

            if (screen_angle_deg < 90) { // Top-right quadrant (Red -> Yellow)
                hsv_angle_deg = ImLerp(0.0f, 60.0f, screen_angle_deg / 90.0f);
            } else if (screen_angle_deg < 180) { // Top-left quadrant (Yellow -> Green)
                hsv_angle_deg = ImLerp(60.0f, 120.0f, (screen_angle_deg - 90.0f) / 90.0f);
            } else if (screen_angle_deg < 270) { // Bottom-left quadrant (Green -> Blue)
                hsv_angle_deg = ImLerp(120.0f, 240.0f, (screen_angle_deg - 180.0f) / 90.0f);
            } else { // Bottom-right quadrant (Blue -> Red)
                hsv_angle_deg = ImLerp(240.0f, 360.0f, (screen_angle_deg - 270.0f) / 90.0f);
            }
            float hue = hsv_angle_deg / 360.0f;
            
            ImVec4 rgb;
            ImGui::ColorConvertHSVtoRGB(hue, 1.0f, 1.0f, rgb.x, rgb.y, rgb.z);
            ImU32 color = IM_COL32(rgb.x * 255, rgb.y * 255, rgb.z * 255, 255);

            draw_list->PathLineTo(center + ImVec2(cosf(a0) * inner_radius, -sinf(a0) * inner_radius));
            draw_list->PathLineTo(center + ImVec2(cosf(a1) * inner_radius, -sinf(a1) * inner_radius));
            draw_list->PathLineTo(center + ImVec2(cosf(a1) * radius, -sinf(a1) * radius));
            draw_list->PathLineTo(center + ImVec2(cosf(a0) * radius, -sinf(a0) * radius));
            draw_list->PathFillConvex(color);
        }
    }

    draw_list->AddCircle(center, radius, IM_COL32(100, 100, 110, 255), 32, 1.0f);
    draw_list->AddLine(center - ImVec2(radius, 0), center + ImVec2(radius, 0), IM_COL32(70, 70, 75, 255));
    draw_list->AddLine(center - ImVec2(0, radius), center + ImVec2(0, radius), IM_COL32(70, 70, 75, 255));
    
    // Draw handle
    ImVec2 handle_pos = center + ImVec2(offset->x, -offset->y) * radius;
    draw_list->AddCircleFilled(handle_pos, 6, IM_COL32(230, 230, 230, 255), 16);
    draw_list->AddCircle(handle_pos, 7, IM_COL32(50, 50, 50, 255), 16, 1.5f);

    // Luma Slider
    ImGui::SameLine();
    ImGui::PushID(id);
    changed |= ImGui::VSliderFloat("##luma", ImVec2(20, radius * 2), luma, -100.0f, 100.0f, "");
    ImGui::PopID();

    return changed;
}

} // namespace


namespace Panes {

bool render_color_wheels(AppState& state) {
    bool changed = false;
    
    float wheel_radius = 50.0f;
    float item_width = wheel_radius * 2 + 30; // wheel + slider
    float window_width = ImGui::GetContentRegionAvail().x;
    
    // Center the controls
    ImGui::SetCursorPosX((window_width - item_width * 3 - ImGui::GetStyle().ItemSpacing.x * 2) * 0.5f);

    ImGui::BeginGroup();
    ImGui::Text("Shadows");
    changed |= ColorWheel("##shadows_wheel", wheel_radius, &state.params.shadows_wheel, &state.params.shadows_luma);
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::Text("Midtones");
    changed |= ColorWheel("##midtones_wheel", wheel_radius, &state.params.midtones_wheel, &state.params.midtones_luma);
    ImGui::EndGroup();

    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::Text("Highlights");
    changed |= ColorWheel("##highlights_wheel", wheel_radius, &state.params.highlights_wheel, &state.params.highlights_luma);
    ImGui::EndGroup();

    return changed;
}

} // namespace Panes
