#ifndef PANE_COLOR_CURVES_H
#define PANE_COLOR_CURVES_H

struct AppState;

namespace Panes {
    // Renders the color grading curves and returns true if any were changed.
    bool render_color_curves(AppState& state);
}

#endif // PANE_COLOR_CURVES_H
