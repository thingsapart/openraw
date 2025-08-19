#ifndef IMGUI_CUSTOM_WIDGETS_H
#define IMGUI_CUSTOM_WIDGETS_H

#include "imgui.h"
#include <string>
#include <vector>

namespace Widgets {

/**
 * @brief A combo box widget with a text filter and right-aligned label.
 *
 * Renders a combo box that takes up the left column of a two-column layout,
 * with its label in the right column. The combo opens a popup with a search
 * filter for long lists.
 *
 * @param label The label to display. If it contains "##", the part after is used as the ID.
 * @param current_item Reference to the string holding the current selection.
 * @param all_items A vector of all possible items.
 * @return True if the selection was changed.
 */
bool ComboWithFilter(const char* label, std::string& current_item, const std::vector<std::string>& all_items);

/**
 * @brief A float slider with its label positioned to the right.
 *
 * Renders a slider in the left column and its label in the right column.
 *
 * @param label The label to display. If it contains "##", the part after is used as the ID.
 * @param v Pointer to the float value.
 * @param v_min Minimum slider value.
 * @param v_max Maximum slider value.
 * @param format Display format for the value.
 * @return True if the value was changed.
 */
bool SliderFloatWithLabel(const char* label, float* v, float v_min, float v_max, const char* format);

/**
 * @brief A drag float widget with its label positioned to the right.
 *
 * Renders a drag float control in the left column and its label in the right column.
 *
 * @param label The label to display. If it contains "##", the part after is used as the ID.
 * @param v Pointer to the float value.
 * @param v_speed Speed of the drag control.
 * @param v_min Minimum value.
 * @param v_max Maximum value.
 * @param format Display format for the value.
 * @return True if the value was changed.
 */
bool DragFloatWithLabel(const char* label, float* v, float v_speed, float v_min, float v_max, const char* format);

} // namespace Widgets

#endif
