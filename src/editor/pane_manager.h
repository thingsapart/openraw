#ifndef PANE_MANAGER_H
#define PANE_MANAGER_H

#include <vector>
#include <string>
#include <functional>
#include <memory>

// Forward-declare AppState to avoid circular includes
struct AppState;

// Define the type for a pane's rendering function
using PaneRenderFunction = std::function<void(AppState&)>;

// Represents a single collapsible pane in the UI
struct Pane {
    std::string title;
    PaneRenderFunction render_func;
    bool default_open;
};

class PaneManager {
public:
    // Registers a new pane. Panes are rendered in the order they are registered.
    void register_pane(const std::string& title, PaneRenderFunction render_func, bool is_static = false, bool default_open = true);
    
    // Renders all registered panes according to their static/scrolling state.
    void render_all_panes(AppState& state);

private:
    std::vector<Pane> static_panes_;
    std::vector<Pane> scrolling_panes_;
};

#endif // PANE_MANAGER_H
