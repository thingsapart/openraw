#ifndef HALIDE_RUNNER_H
#define HALIDE_RUNNER_H

#include "app_state.h"
#include "color_tools.h"
#include "tone_curve_utils.h"

// Runs both the main and thumbnail Halide pipelines based on the current
// application state. This is a synchronous, blocking call.
void RunHalidePipelines(AppState& state);

#endif // HALIDE_RUNNER_H
