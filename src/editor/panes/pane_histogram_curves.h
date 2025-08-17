#ifndef PANE_HISTOGRAM_CURVES_H
#define PANE_HISTOGRAM_CURVES_H

struct AppState;

namespace Panes {
    // Renders the combined histogram and tone curve editor pane.
    // Returns true if any curve was modified by the user.
    bool render_histogram_curves(AppState& state);
}

#endif // PANE_HISTOGRAM_CURVES_H
