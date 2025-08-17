#ifndef PANE_LENS_CORRECTION_H
#define PANE_LENS_CORRECTION_H

struct AppState;

namespace Panes {
    // Renders the UI for the "Vignette" pane.
    // Returns true if any parameter was changed.
    bool render_vignette(AppState& state);
}

#endif // PANE_LENS_CORRECTION_H
