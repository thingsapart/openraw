#ifndef PROCESS_OPTIONS_H
#define PROCESS_OPTIONS_H

#include <string>
#include <vector>
#include <map>
#include <stdexcept>

// All pipeline parameters are now encapsulated in this single struct.
// It is shared between the command-line runner and the new UI editor.
struct ProcessConfig {
    std::string input_path;
    std::string output_path;
    std::string demosaic_algorithm = "fast";
    float downscale_factor = 1.0f;
    float color_temp = 3700.0f;
    float tint = 0.0f;
    float ca_strength = 0.0f;
    int timing_iterations = 5;

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
    std::string curve_points_str;
    std::string curve_r_str;
    std::string curve_g_str;
    std::string curve_b_str;
    int curve_mode = 1; // 0=Luma, 1=RGB
};


// Parses command line arguments and populates a ProcessConfig struct.
// Throws std::runtime_error on parsing failure.
ProcessConfig parse_args(int argc, char **argv);

// Prints the command-line usage instructions to stdout.
void print_usage();


#endif // PROCESS_OPTIONS_H

