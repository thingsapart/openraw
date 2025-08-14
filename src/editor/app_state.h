#ifndef APP_STATE_H
#define APP_STATE_H

#include "process_options.h"

// UI State management struct
// This is the central data structure passed to UI rendering functions.
struct AppState {
    ProcessConfig params;
    // UI specific state
    float zoom = 1.0f;
    // In a real app, this would hold texture IDs, etc.
};

#endif // APP_STATE_H
