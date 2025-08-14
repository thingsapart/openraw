#include "lvgl.h"
#include "sdl_viewer.h"

int main(int argc, char *argv[]) {
    // These arguments are not used in this simple example.
    (void)argc;
    (void)argv;

    // Initialize LVGL, SDL, and the display driver using the v9-compatible helper.
    if (sdl_viewer_init() != 0) {
        // Initialization failed, nothing more to do.
        return -1;
    }

    // Get a pointer to the active screen. lv_scr_act() is a compatible macro for lv_screen_active().
    lv_obj_t *scr = sdl_viewer_create_main_screen();

    // Create a label widget on the screen.
    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, "Hello world");

    // Center the label on the screen.
    lv_obj_center(label);

    // Run the main event loop. This function contains the required lv_timer_handler() for v9.
    sdl_viewer_loop();

    // Although this part is unreachable because of the infinite loop,
    // it's good practice to have a return statement.
    return 0;
}
