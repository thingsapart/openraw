#ifndef PANE_DEHAZE_H
#define PANE_DEHAZE_H

struct AppState;

namespace Panes {
    // Renders the UI for the "Dehaze" pane.
    // Returns true if any parameter was changed.
    bool render_dehaze(AppState& state);
}

#endif // PANE_DEHAZE_H
