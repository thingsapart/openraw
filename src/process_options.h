#ifndef PROCESS_OPTIONS_H
#define PROCESS_OPTIONS_H

#include <string>
#include <vector>
#include <map>
#include <stdexcept>

// A simple struct to represent a 2D point, used for curve controls.
// This is now the canonical representation used in ProcessConfig.
struct Point { float x, y; };

// All pipeline parameters are now encapsulated in this single struct.
// It is shared between the command-line runner and the new UI editor.
struct ProcessConfig {
    std::string input_path;
    std::string output_path;
    std::string demosaic_algorithm = "fast";
    float downscale_factor = 1.0f;
    float color_temp = 3700.0f;
    float tint = 0.0f;
    float exposure = 0.0f; // in stops. Default to 0.0 (no change)
    float ca_strength = 0.0f;
    int timing_iterations = 5;

    // Dehaze
    float dehaze_strength = 0.0f;

    // Denoise
    float denoise_strength = 50.0f;
    float denoise_eps = 0.01f;

    // Local Laplacian
    float ll_detail = 0.0f;
    float ll_clarity = 0.0f;
    float ll_shadows = 0.0f;
    float ll_highlights = 0.0f;
    float ll_blacks = 0.0f;
    float ll_whites = 0.0f;

    // Tone Mapping
    int tonemap_algorithm = 3; // 0=linear, 1=reinhard, 2=filmic, 3=gamma
    float gamma = 2.2f;
    float contrast = 50.0f;

    // Curve points are now stored as vectors of points after parsing.
    // "luma" is the fallback/master, the others are overrides.
    std::vector<Point> curve_points_luma;
    std::vector<Point> curve_points_r;
    std::vector<Point> curve_points_g;
    std::vector<Point> curve_points_b;

    int curve_mode = 1; // 0=Luma, 1=RGB

    // --- New Color Grading parameters ---
    Point shadows_wheel = {0.f, 0.f};
    float shadows_luma = 0.f;
    Point midtones_wheel = {0.f, 0.f};
    float midtones_luma = 0.f;
    Point highlights_wheel = {0.f, 0.f};
    float highlights_luma = 0.f;

    std::vector<Point> curve_hue_vs_hue;
    std::vector<Point> curve_hue_vs_sat;
    std::vector<Point> curve_hue_vs_lum;
    std::vector<Point> curve_lum_vs_sat;
    std::vector<Point> curve_sat_vs_sat;
    
    // --- Lens Correction ---
    float vignette_amount = 0.0f; // Range [-100, 100] in UI
    float vignette_midpoint = 50.0f; // Range [0, 100] in UI
    float vignette_roundness = 100.0f; // Range [0, 100] in UI
    float vignette_highlights = 0.0f; // Range [0, 100] in UI
};


// Parses command line arguments and populates a ProcessConfig struct.
// Throws std::runtime_error on parsing failure.
ProcessConfig parse_args(int argc, char **argv);

// Prints the command-line usage instructions to stdout.
void print_usage();


#endif // PROCESS_OPTIONS_H

