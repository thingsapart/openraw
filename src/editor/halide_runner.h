#ifndef HALIDE_RUNNER_H
#define HALIDE_RUNNER_H

#include "app_state.h"
#include "imgui.h"

// Runs both the main and thumbnail Halide pipelines based on the current
// application state.
void RunHalidePipelines(AppState& state);

#endif // HALIDE_RUNNER_H
