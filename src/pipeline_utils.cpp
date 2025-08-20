#include "pipeline_utils.h"
#include <cmath>
#include <iostream>
#include <algorithm> // For std::max/min

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace PipelineUtils {

// --- White Balance ---
RGBGains kelvin_to_rgb_gains(float temp, float tint) {
    // This is a standard algorithm for converting temperature to RGB multipliers,
    // based on the method used in dcraw.
    double r, g, b;
    double temp_d = temp;

    if (temp_d <= 6600.0) {
        r = 255.0;
        double g_temp = temp_d / 100.0;
        g = 99.4708025861 * log(g_temp) - 161.1195681661;

        if (temp_d <= 1900.0) {
            b = 0;
        } else {
            double b_temp = (temp_d - 600.0) / 100.0;
            b = 138.5177312231 * log(b_temp) - 305.0447927307;
        }
    } else {
        double r_temp = (temp_d - 6000.0) / 100.0;
        r = 329.698727446 * pow(r_temp, -0.1332047592);
        
        double g_temp = (temp_d - 6000.0) / 100.0;
        g = 288.1221695283 * pow(g_temp, -0.0755148492);

        b = 255.0;
    }

    r = std::max(0.0, std::min(255.0, r));
    g = std::max(0.0, std::min(255.0, g));
    b = std::max(0.0, std::min(255.0, b));
    
    // Apply tint, which primarily adjusts the green channel.
    // A positive tint value should shift towards magenta (less green).
    g *= (1.0 - tint * 0.5);

    // To white balance, we need gains that are inversely proportional to the light color.
    // We normalize these gains so the green channel multiplier is 1.0.
    RGBGains final_gains;
    if (r > 1e-6 && g > 1e-6 && b > 1e-6) {
        final_gains.r = static_cast<float>(g / r);
        final_gains.g = 1.0f;
        final_gains.b = static_cast<float>(g / b);
    } else {
        // Avoid division by zero in extreme cases, return neutral gains.
        final_gains.r = 1.0f;
        final_gains.g = 1.0f;
        final_gains.b = 1.0f;
    }

    return final_gains;
}


// --- Lens Correction LUT Generation ---
namespace LensCorrection {
    const int LUT_SIZE = 2048;
    const float MAX_RD_SQUARED_NORM = 3.0f;

    // Solves the depressed cubic equation: r_u^3 + p*r_u + q = 0
    // using Cardano's method. We only need the single, positive real root.
    float solve_cubic_poly3(float p, float q) {
        float p_3 = p / 3.0f;
        float q_2 = q / 2.0f;
        float discriminant = q_2 * q_2 + p_3 * p_3 * p_3;

        if (discriminant >= 0) {
            float root_discriminant = sqrtf(discriminant);
            float term1 = cbrtf(-q_2 + root_discriminant);
            float term2 = cbrtf(-q_2 - root_discriminant);
            return term1 + term2;
        } else {
            float r = sqrtf(-p_3 * -p_3 * -p_3);
            float phi = acosf(-q_2 / r);
            float two_sqrt_p3 = 2.0f * sqrtf(-p_3);
            return two_sqrt_p3 * cosf(phi / 3.0f);
        }
    }

    // Solves for r_u using Newton-Raphson for the POLY5 model.
    // f(r_u) = k2*r_u^5 + k1*r_u^3 + r_u - r_d = 0
    float solve_poly5(float k1, float k2, float rd) {
        float ru = rd; // Initial guess
        for (int i = 0; i < 4; ++i) { // 4 iterations is plenty
            float ru2 = ru * ru;
            float ru3 = ru2 * ru;
            float ru4 = ru2 * ru2;
            float ru5 = ru4 * ru;
            float f = k2 * ru5 + k1 * ru3 + ru - rd;
            float f_prime = 5.0f * k2 * ru4 + 3.0f * k1 * ru2 + 1.0f;
            if (fabsf(f_prime) < 1e-6) break; // Avoid division by zero
            ru -= f / f_prime;
        }
        return ru;
    }

    // Solves for r_u using Newton-Raphson for the PTLENS model.
    // f(r_u) = a*r_u^4 + b*r_u^3 + c*r_u^2 + (1-a-b-c)*r_u - r_d = 0
    float solve_ptlens(float a, float b, float c, float rd) {
        float ru = rd; // Initial guess
        float d = 1.0f - a - b - c;
        for (int i = 0; i < 4; ++i) { // 4 iterations is plenty
            float ru2 = ru * ru;
            float ru3 = ru2 * ru;
            float ru4 = ru2 * ru2;
            float f = a * ru4 + b * ru3 + c * ru2 + d * ru - rd;
            float f_prime = 4.0f * a * ru3 + 3.0f * b * ru2 + 2.0f * c * ru + d;
            if (fabsf(f_prime) < 1e-6) break; // Avoid division by zero
            ru -= f / f_prime;
        }
        return ru;
    }

    // Generates an "identity" LUT for when no correction is needed.
    Halide::Runtime::Buffer<float, 1> generate_identity_lut() {
        Halide::Runtime::Buffer<float, 1> lut(LUT_SIZE);
        for (int i = 0; i < LUT_SIZE; ++i) {
            lut(i) = 1.0f;
        }
        return lut;
    }

#ifdef USE_LENSFUN
    Halide::Runtime::Buffer<float, 1> generate_distortion_lut(const lfLensCalibDistortion& model) {
        Halide::Runtime::Buffer<float, 1> lut(LUT_SIZE);

        for (int i = 0; i < LUT_SIZE; ++i) {
            float rd_sq_norm = (float)i * MAX_RD_SQUARED_NORM / (float)(LUT_SIZE - 1);
            float rd_norm = sqrtf(rd_sq_norm);
            float ru_norm = rd_norm; // Default to no change

            switch (model.Model) {
                case LF_DIST_MODEL_POLY3: {
                    float k1 = model.Terms[0];
                    if (fabsf(k1) > 1e-6f) {
                        ru_norm = solve_cubic_poly3((1.0f - k1) / k1, -rd_norm / k1);
                    }
                    break;
                }
                case LF_DIST_MODEL_POLY5: {
                    ru_norm = solve_poly5(model.Terms[0], model.Terms[1], rd_norm);
                    break;
                }
                case LF_DIST_MODEL_PTLENS: {
                    ru_norm = solve_ptlens(model.Terms[0], model.Terms[1], model.Terms[2], rd_norm);
                    break;
                }
                default: break;
            }
            if (rd_norm > 1e-5f) {
                lut(i) = ru_norm / rd_norm;
            } else {
                lut(i) = 1.0f; // Avoid division by zero at the center
            }
        }
        return lut;
    }
#endif // USE_LENSFUN

} // namespace LensCorrection


void prepare_color_matrices(const RawImageData& raw_data,
                            Halide::Runtime::Buffer<float, 2>& matrix_3200,
                            Halide::Runtime::Buffer<float, 2>& matrix_7000)
{
    float inv_range = 1.0f;
    if (raw_data.white_level > raw_data.black_level) {
        inv_range = 1.0f / (static_cast<float>(raw_data.white_level) - static_cast<float>(raw_data.black_level));
    }
    
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) { // Copy 3x3 rotational part
            matrix_3200(j, i) = raw_data.matrix_3200[i][j];
            matrix_7000(j, i) = raw_data.matrix_7000[i][j];
        }
        // Normalize the 4th (offset) column
        matrix_3200(3, i) = raw_data.matrix_3200[i][3] * inv_range;
        matrix_7000(3, i) = raw_data.matrix_7000[i][3] * inv_range;
    }
}

void get_interpolated_color_matrix(const RawImageData& raw_data,
                                   float color_temp,
                                   Halide::Runtime::Buffer<float, 2>& output_matrix)
{
    Halide::Runtime::Buffer<float, 2> matrix_3200(4, 3), matrix_7000(4, 3);
    prepare_color_matrices(raw_data, matrix_3200, matrix_7000);

    // If the file doesn't have matrices, or for extreme temperatures, don't interpolate.
    if (!raw_data.has_matrix || color_temp <= 3200.0f) {
        output_matrix.copy_from(matrix_3200);
        return;
    }
    if (color_temp >= 7000.0f) {
        output_matrix.copy_from(matrix_7000);
        return;
    }

    // Interpolation weight 'alpha' is 0 at 3200K and 1 at 7000K.
    float alpha = (color_temp - 3200.0f) / (7000.0f - 3200.0f);

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            output_matrix(j, i) = (1.0f - alpha) * matrix_3200(j, i) + alpha * matrix_7000(j, i);
        }
    }
}

} // namespace PipelineUtils

