#ifndef PANE_LENS_CORRECTION_H
#define PANE_LENS_CORRECTION_H

struct AppState;

namespace Panes {
    // Renders the UI for the "Lens Corrections" pane, including profile selection,
    // vignette, and geometry controls.
    // Returns true if any parameter was changed.
    bool render_lens_correction(AppState& state);
}

#endif // PANE_LENS_CORRECTION_H
