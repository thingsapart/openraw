#ifndef PANE_COLOR_WHEELS_H
#define PANE_COLOR_WHEELS_H

struct AppState;

namespace Panes {
    // Renders the color wheels and returns true if any parameter was changed.
    bool render_color_wheels(AppState& state);
}

#endif // PANE_COLOR_WHEELS_H
