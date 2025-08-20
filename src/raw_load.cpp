#include "raw_load.h"
#include "librawspeed/RawSpeed-API.h"
#include <memory>
#include <stdexcept>
#include <cstring> // For memcpy
#include <numeric> // For std::accumulate
#include <cmath>   // For fabsf
#include "halide_image_io.h"
#include "simple_timer.h" // Include the new timer header

// RawSpeed requires this function to be defined by the user.
// It tells the library how many threads it's allowed to use.
// We'll return 1 to keep it single-threaded, as Halide handles our parallelism.
int rawspeed_get_number_of_processor_cores() {
    return 1;
}

using namespace Halide::Runtime;
using namespace Halide::Tools;

namespace { // Anonymous namespace for local helpers

// Default DNG color matrices, used as a fallback.
const float default_matrix_3200[3][4] = {{1.6697f, -0.2693f, -0.4004f, -42.4346f},
                                         {-0.3576f, 1.0615f, 1.5949f, -37.1158f},
                                         {-0.2175f, -1.8751f, 6.9640f, -26.6970f}};

const float default_matrix_7000[3][4] = {{2.2997f, -0.4478f, 0.1706f, -39.0923f},
                                         {-0.3826f, 1.5906f, -0.2080f, -25.4311f},
                                         {-0.0888f, -0.7344f, 2.2832f, -20.0826f}};

// Inverts a 3x3 matrix.
void invert3x3(const float in[3][3], float out[3][3]) {
    float det = in[0][0] * (in[1][1] * in[2][2] - in[2][1] * in[1][2]) -
                in[0][1] * (in[1][0] * in[2][2] - in[1][2] * in[2][0]) +
                in[0][2] * (in[1][0] * in[2][1] - in[1][1] * in[2][0]);

    if (fabsf(det) < 1e-8) {
        // Matrix is not invertible, return identity
        for(int i=0; i<3; ++i) for(int j=0; j<3; ++j) out[i][j] = (i==j);
        return;
    }

    float invdet = 1.0f / det;

    out[0][0] = (in[1][1] * in[2][2] - in[2][1] * in[1][2]) * invdet;
    out[0][1] = (in[0][2] * in[2][1] - in[0][1] * in[2][2]) * invdet;
    out[0][2] = (in[0][1] * in[1][2] - in[0][2] * in[1][1]) * invdet;
    out[1][0] = (in[1][2] * in[2][0] - in[1][0] * in[2][2]) * invdet;
    out[1][1] = (in[0][0] * in[2][2] - in[0][2] * in[2][0]) * invdet;
    out[1][2] = (in[1][0] * in[0][2] - in[0][0] * in[1][2]) * invdet;
    out[2][0] = (in[1][0] * in[2][1] - in[2][0] * in[1][1]) * invdet;
    out[2][1] = (in[2][0] * in[0][1] - in[0][0] * in[2][1]) * invdet;
    out[2][2] = (in[0][0] * in[1][1] - in[1][0] * in[0][1]) * invdet;
}


// Helper to map RawSpeed's CFA enum to our integer pattern codes
int map_cfa_pattern(const rawspeed::ColorFilterArray& cfa) {
    // Check the pattern of the top-left 2x2 quad
    rawspeed::CFAColor c00 = cfa.getColorAt(0, 0);
    rawspeed::CFAColor c10 = cfa.getColorAt(1, 0);
    rawspeed::CFAColor c01 = cfa.getColorAt(0, 1);

    using C = rawspeed::CFAColor;
    if (c00 == C::GREEN && c10 == C::RED && c01 == C::BLUE) return 0;   // GRBG
    if (c00 == C::RED   && c10 == C::GREEN && c01 == C::GREEN) return 1;  // RGGB
    if (c00 == C::GREEN && c10 == C::BLUE && c01 == C::RED) return 2;    // GBRG
    if (c00 == C::BLUE  && c10 == C::GREEN && c01 == C::GREEN) return 3;  // BGGR
    if (c00 == C::RED   && c10 == C::GREEN && c01 == C::BLUE) return 4;   // RGBG (Sony)

    fprintf(stderr, "Warning: Unknown CFA pattern from RawSpeed. Defaulting to GRBG.\n");
    return 0;
}

// A singleton-like holder for the CameraMetaData
std::unique_ptr<rawspeed::CameraMetaData> gCameraMetaData;

} // namespace

RawImageData load_raw(const std::string &path) {
    RawImageData result;

    try {
        if (!gCameraMetaData) {
            SimpleTimer meta_timer("RawSpeed Metadata Load");
            try {
                gCameraMetaData = std::make_unique<rawspeed::CameraMetaData>("../share/rawspeed/cameras.xml");
            } catch (const rawspeed::RawspeedException&) {
                gCameraMetaData = std::make_unique<rawspeed::CameraMetaData>("./data/cameras.xml");
            }
        }

        auto [file_owner, buffer] = [] (const std::string& p) {
            SimpleTimer read_timer("File Read to Buffer");
            rawspeed::FileReader reader(p.c_str());
            return reader.readFile();
        }(path);

        std::unique_ptr<rawspeed::RawDecoder> decoder;
        {
            SimpleTimer parse_timer("RawSpeed Parse");
            rawspeed::RawParser parser(buffer);
            decoder = parser.getDecoder();
        }

        if (!decoder) {
            throw std::runtime_error("RawSpeed Error: Failed to get decoder for " + path);
        }

        rawspeed::RawImage img = [&] {
            SimpleTimer decode_timer("RawSpeed Decode");
            decoder->failOnUnknown = false;
            decoder->checkSupport(gCameraMetaData.get());
            decoder->decodeRaw();
            decoder->decodeMetaData(gCameraMetaData.get());
            rawspeed::RawImage decoded_img = decoder->mRaw;
            decoded_img->scaleBlackWhite();
            return decoded_img;
        }();

        // Get the cropped 2D array view. This object is the ground truth for dimensions and access.
        rawspeed::CroppedArray2DRef<uint16_t> pixels = img->getU16DataAsCroppedArray2DRef();

        // Use the correct public members from the view to get dimensions.
        int width = pixels.croppedWidth;
        int height = pixels.croppedHeight;

        result.cfa_pattern = map_cfa_pattern(img->cfa);
        result.black_level = 0;
        result.white_level = 65535;
        result.bayer_data = Buffer<uint16_t, 2>(width, height);

        {
            SimpleTimer copy_timer("Pixel Buffer Copy");
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    result.bayer_data(x, y) = pixels(y, x);
                }
            }
        }

        const rawspeed::Camera* cam = gCameraMetaData->getCamera(img->metadata.make, img->metadata.model, img->metadata.mode);
        float cam_to_xyz[3][3];
        bool matrix_found = false;

        // Priority 1: Image-specific XYZ -> Camera matrix (needs inversion).
        if (!img->metadata.colorMatrix.empty() && img->metadata.colorMatrix.size() >= 9) {
            float xyz_to_cam[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    const auto& rational = img->metadata.colorMatrix[i * 3 + j];
                    xyz_to_cam[i][j] = static_cast<float>(rational.num) / rational.den;
                }
            }
            invert3x3(xyz_to_cam, cam_to_xyz);
            matrix_found = true;
            result.has_matrix = true;
        }
        // Priority 2: Generic Camera -> XYZ matrix from database (no inversion needed).
        else if (cam && !cam->color_matrix.empty() && cam->color_matrix.size() >= 9) {
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    const auto& rational = cam->color_matrix[i * 3 + j];
                    cam_to_xyz[i][j] = static_cast<float>(rational.num) / rational.den;
                }
            }
            matrix_found = true;
            result.has_matrix = true;
        }

        if (matrix_found) {
            // We have a Camera -> XYZ matrix. Now convert it to Camera -> sRGB.
            const float xyz_to_srgb_d65[3][3] = {
                { 3.2404542f, -1.5371385f, -0.4985314f},
                {-0.9692660f,  1.8760108f,  0.0415560f},
                { 0.0556434f, -0.2040259f,  1.0572252f}
            };

            float cam_to_srgb[3][3] = {{0}};
            for (int i = 0; i < 3; ++i) { // output row
                for (int j = 0; j < 3; ++j) { // output col
                    for (int k = 0; k < 3; ++k) { // inner dim
                        cam_to_srgb[i][j] += xyz_to_srgb_d65[i][k] * cam_to_xyz[k][j];
                    }
                }
            }

            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    result.matrix_3200[i][j] = cam_to_srgb[i][j];
                    result.matrix_7000[i][j] = cam_to_srgb[i][j];
                }
                result.matrix_3200[i][3] = 0.0f;
                result.matrix_7000[i][3] = 0.0f;
            }
        } else {
            // Priority 3: Fall back to hardcoded DNG defaults.
            result.has_matrix = false;
            memcpy(result.matrix_3200, default_matrix_3200, sizeof(float) * 12);
            memcpy(result.matrix_7000, default_matrix_7000, sizeof(float) * 12);
        }

    } catch (const rawspeed::RawspeedException& e) {
        throw std::runtime_error("RawSpeed Error: " + std::string(e.what()));
    }

    return result;
}

RawImageData load_raw_png(const std::string &path) {
    SimpleTimer png_timer("PNG Load and Convert");
    RawImageData result;

    result.bayer_data = load_and_convert_image(path);

    result.cfa_pattern = 0;
    result.black_level = 25;
    result.white_level = 1023;
    result.has_matrix = false;
    memcpy(result.matrix_3200, default_matrix_3200, sizeof(float) * 12);
    memcpy(result.matrix_7000, default_matrix_7000, sizeof(float) * 12);

    return result;
}
