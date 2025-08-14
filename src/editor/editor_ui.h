#ifndef EDITOR_UI_H
#define EDITOR_UI_H

// Forward-declare the AppState struct to avoid circular dependencies
struct AppState;

// Renders the entire user interface for the application.
void RenderUI(AppState& state);

#endif // EDITOR_UI_H
