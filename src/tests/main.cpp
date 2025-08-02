#include "test_harness.h"

// Define the global failure counter
int test_failures = 0;

// Declare the test functions from other files
void test_color_correct_behavior();
void test_demosaic_behavior();
void test_saturation_out_of_gamut();
void test_saturation_on_dark_colors(); 
void test_hot_pixel_propagation();
void test_e2e_linear_curve();
void test_e2e_inverting_curve();
void test_e2e_crushing_curve();
void test_default_s_curve_range();
void test_ca_correction_artifacts(); 
void test_dump_intermediate_stages(); // New declaration


int main(int argc, char **argv) {
    std::cout << "Starting RAW pipeline tests...\n" << std::endl;

    // This test is for manual debugging and should be run when needed.
    // It requires a user-provided image.
    test_dump_intermediate_stages();

    // Standard unit tests
    test_color_correct_behavior();
    test_demosaic_behavior();
    test_saturation_out_of_gamut();
    test_saturation_on_dark_colors();
    test_hot_pixel_propagation();
    test_e2e_linear_curve();
    test_e2e_inverting_curve();
    test_e2e_crushing_curve();
    test_default_s_curve_range();
    test_ca_correction_artifacts();


    std::cout << "\n-------------------------------------\n";
    if (test_failures == 0) {
        std::cout << "All tests passed successfully!" << std::endl;
        return 0;
    } else {
        std::cout << test_failures << " test(s) failed." << std::endl;
        return 1;
    }
}
