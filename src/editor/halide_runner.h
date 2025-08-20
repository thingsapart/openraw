#ifndef EDITOR_HALIDE_RUNNER_H
#define EDITOR_HALIDE_RUNNER_H

// Forward-declare the AppState struct to avoid circular dependencies and redefinition errors.
// The full definition will be included in the .cpp file.
struct AppState;

// Main function to execute the Halide pipelines for preview and thumbnail.
void RunHalidePipelines(AppState& state);

#endif // EDITOR_HALIDE_RUNNER_H
