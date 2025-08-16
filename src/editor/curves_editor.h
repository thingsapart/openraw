#ifndef CURVES_EDITOR_H
#define CURVES_EDITOR_H

#include "process_options.h" // For Point
#include "HalideBuffer.h"

// Enable extra ImGui math operators like '+' and ImLengthSqr()
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <vector>
#include <cstdint>

/**
 * @class CurvesEditor
 * @brief A self-contained, interactive spline curve editor widget for ImGui.
 *
 * This class implements a reusable UI component for manipulating curves defined by
 * a series of control points. It is designed to be highly configurable to support
 * different types of curves (e.g., tone curves, color grading curves).
 *
 * ### Core Functionality
 * The editor operates on a `std::vector<Point>` passed by reference. It visualizes
 * the curve using a monotone-preserving cubic Hermite spline (`ToneCurveUtils::Spline`)
 * and provides direct manipulation of the control points. Its internal state (like
 * which point is being dragged) is managed statically to persist across frames.
 *
 * ### Interaction Model
 * - **Add & Drag Point:** Press the Left Mouse Button on an empty area of the canvas
 *   to create a new point and immediately begin dragging it.
 * - **Remove Point:** Single Right-Click on an existing control point.
 * - **Move Point:** Left-Click and Drag an existing control point. The hitbox for
 *   each point is a 5-pixel radius halo.
 *
 * ### Coordinate System
 * The widget maps between two coordinate systems:
 * 1.  **Logical Coordinates:** The curve's native space. The X-axis is always [0, 1].
 *     The Y-axis is defined by the `y_min` and `y_max` parameters.
 * 2.  **Screen Coordinates:** The pixel space of the canvas on the UI.
 * The mapping is handled internally.
 *
 * ### Configuration Parameters (passed to `render`)
 * - `id`: A unique string identifier for the widget instance.
 * - `points`: The vector of control points to manipulate.
 * - `size`: The pixel dimensions of the widget's canvas.
 * - `default_y`: The default Y-value for a flat, horizontal curve.
 * - `is_additive`: If true, the default spline is y=0. If false, it's y=1.
 * - `is_identity`: If true, the default spline is y=x. This flag also enables
 *   special behaviors for tone curves.
 * - `y_min`, `y_max`: Defines the logical vertical range of the editor.
 *
 * ### Special Behaviors for Tone Curves (`is_identity = true`)
 * - **Monotonicity:** The underlying spline enforces monotonicity to prevent
 *   inversions in the tone curve.
 * - **Endpoint Constraints:** The first and last points are locked to x=0 and x=1
 *   respectively and cannot be deleted. Their Y-value can be adjusted.
 * - **Visual Flattening:** Even if a point's logical Y-value is dragged outside
 *   the [0, 1] range, its visual handle is clamped to the canvas boundary. A faint
 *   "ghost line" is drawn to the point's true logical position, providing clear
 *   feedback for clipping or crushing blacks/whites.
 * - **Y-Axis Clamping:** Only the endpoints of the tone curve can be dragged
 *   vertically out of the visual bounds. Interior points are clamped.
 */
class CurvesEditor {
public:
    // Renders the interactive curve editor widget.
    static bool render(const char* id, std::vector<Point>& points, const ImVec2& size,
                       float default_y, bool is_additive, bool is_identity, float y_min, float y_max);

    // Static helper to draw a curve from its control points without interaction.
    static void draw_readonly_spline(ImDrawList* draw_list, const std::vector<Point>& points,
                                     float default_y, bool is_additive, bool is_identity,
                                     const ImVec2& canvas_pos, const ImVec2& canvas_size, ImU32 color,
                                     float y_min, float y_max);
};


#endif // CURVES_EDITOR_H
