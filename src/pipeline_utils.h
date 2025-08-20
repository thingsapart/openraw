#ifndef PIPELINE_UTILS_H
#define PIPELINE_UTILS_H

#include "HalideBuffer.h"
#include "raw_load.h"

// To enable Lensfun support, compile with -DUSE_LENSFUN and link against liblensfun.
#ifdef USE_LENSFUN
#include "lensfun/lensfun.h"
#endif

namespace PipelineUtils {

// --- White Balance ---
struct RGBGains { float r, g, b; };
// Calculates white balance gains for a given temperature and tint.
RGBGains kelvin_to_rgb_gains(float temp, float tint);


// --- Lens Correction LUT Generation ---
namespace LensCorrection {
    Halide::Runtime::Buffer<float, 1> generate_identity_lut();

#ifdef USE_LENSFUN
    // Generates a distortion correction LUT from a lensfun model.
    Halide::Runtime::Buffer<float, 1> generate_distortion_lut(const lfLensCalibDistortion& model);
#endif
} // namespace LensCorrection

// Prepares the 3200K and 7000K color matrices for the Halide pipeline.
void prepare_color_matrices(const RawImageData& raw_data,
                            Halide::Runtime::Buffer<float, 2>& matrix_3200,
                            Halide::Runtime::Buffer<float, 2>& matrix_7000);

// Interpolates between two color matrices based on color temperature and populates
// a single output matrix for the Halide pipeline.
void get_interpolated_color_matrix(const RawImageData& raw_data,
                                   float color_temp,
                                   Halide::Runtime::Buffer<float, 2>& output_matrix);

} // namespace PipelineUtils

#endif // PIPELINE_UTILS_H

