#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <algorithm>

#ifdef USE_LIBCAMERA
#include <libcamera/libcamera.h>
#include <png.h>

using namespace libcamera;

// --- Configuration Struct ---
struct Config {
    std::string output_path;
    int width = 0, height = 0;
    unsigned int bit_depth = 0;
    int exposure_us = 0; // 0 means auto
};

// --- PNG Writing Helper ---
bool save_raw_as_png(const std::string& filename, uint32_t width, uint32_t height, uint32_t stride, const uint8_t* data) {
    FILE *fp = fopen(filename.c_str(), "wb");
    if (!fp) {
        std::cerr << "Error: Could not open " << filename << " for writing." << std::endl;
        return false;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) {
        fclose(fp);
        return false;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, nullptr);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        std::cerr << "Error during PNG creation." << std::endl;
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return false;
    }

    png_init_io(png_ptr, fp);

    // Write header. We save the single-channel RAW data as a 16-bit grayscale PNG.
    png_set_IHDR(png_ptr, info_ptr, width, height,
                 16, // We use a 16-bit container for all raw bit depths (10, 12, 14, etc)
                 PNG_COLOR_TYPE_GRAY,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    png_write_info(png_ptr, info_ptr);

    // libpng expects an array of row pointers.
    std::vector<png_bytep> row_pointers(height);
    for (uint32_t y = 0; y < height; ++y) {
        // The data is already in the correct 16-bit unpacked format (2 bytes per pixel)
        row_pointers[y] = (png_bytep)(data + y * stride);
    }

    png_write_image(png_ptr, row_pointers.data());
    png_write_end(png_ptr, nullptr);

    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);

    std::cout << "Successfully saved RAW image to " << filename << std::endl;
    return true;
}

// --- Main Capture Logic ---
void print_usage() {
    std::cout << "Usage: ./capture [options] <output.png>\n\n"
              << "Options:\n"
              << "  --width <w>      Request specific width\n"
              << "  --height <h>     Request specific height\n"
              << "  --bit-depth <d>  Request specific bit depth (e.g., 10, 12)\n"
              << "  --exposure <us>  Set manual exposure time in microseconds (e.g., 33333 for 1/30s).\n"
              << "                   Default is auto-exposure.\n"
              << "  --help           Display this help message\n"
              << std::endl;
}

int main(int argc, char **argv) {
    Config cfg;
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(); return 0; }
        if (arg.rfind("--", 0) == 0) {
            if (i + 1 < argc) args[arg.substr(2)] = argv[++i];
        } else {
            cfg.output_path = arg;
        }
    }

    if (cfg.output_path.empty()) {
        std::cerr << "Error: Output file path is required." << std::endl;
        print_usage();
        return 1;
    }

    try {
        if (args.count("width")) cfg.width = std::stoi(args["width"]);
        if (args.count("height")) cfg.height = std::stoi(args["height"]);
        if (args.count("bit-depth")) cfg.bit_depth = std::stoul(args["bit-depth"]);
        if (args.count("exposure")) cfg.exposure_us = std::stoi(args["exposure"]);
    } catch (const std::exception& e) {
        std::cerr << "Error parsing arguments: " << e.what() << std::endl; return 1;
    }

    auto cm = std::make_unique<CameraManager>();
    cm->start();
    if (cm->cameras().empty()) { std::cerr << "No cameras found" << std::endl; return 1; }

    std::shared_ptr<Camera> camera = cm->cameras()[0];
    camera->acquire();

    PixelFormat best_format;
    unsigned int max_bit_depth = 0;
    for (const auto &format : camera->sensor()->properties().get(properties::PixelFormats)) {
        if (!format.isBayer()) continue;
        if (format.isPacked()) continue;

        if (cfg.bit_depth > 0) {
            if (format.bitdepth() == cfg.bit_depth) {
                best_format = format;
                break;
            }
        } else {
            if (format.bitdepth() > max_bit_depth) {
                max_bit_depth = format.bitdepth();
                best_format = format;
            }
        }
    }

    if (!best_format.isValid()) {
        std::cerr << "Could not find a suitable unpacked RAW format." << std::endl;
        return 1;
    }

    std::unique_ptr<CameraConfiguration> config = camera->generateConfiguration({ StreamRole::Raw });
    StreamConfiguration &streamConfig = config->at(0);
    streamConfig.pixelFormat = best_format;

    if (cfg.width > 0 && cfg.height > 0) {
        streamConfig.size = Size(cfg.width, cfg.height);
    } else {
        Size best_size(0, 0);
        for(const auto& size : streamConfig.formats().at(0).sizes()) {
            if (size.width * size.height > best_size.width * best_size.height) {
                best_size = size;
            }
        }
        streamConfig.size = best_size;
    }

    std::cout << "Requesting format: " << streamConfig.pixelFormat.toString()
              << " at " << streamConfig.size.toString() << std::endl;

    config->validate();
    camera->configure(config->get());

    FrameBufferAllocator allocator(camera);
    allocator.allocate(streamConfig.stream());
    const auto &buffers = allocator.buffers(streamConfig.stream());
    Request *request = camera->createRequest();
    request->addBuffer(streamConfig.stream(), buffers[0].get());

    ControlList &controls = request->controls();
    if (cfg.exposure_us > 0) {
        controls.set(controls::AeEnable, false);
        controls.set(controls::AwbEnable, false);
        controls.set(controls::AnalogueGain, 1.0);
        controls.set(controls::ExposureTime, cfg.exposure_us);
    } else {
        controls.set(controls::AeEnable, true);
        controls.set(controls::AwbEnable, true);
    }

    camera->start();
    camera->queueRequest(request);

    bool captured = false;
    while(!captured) {
        CompletedRequest *completed_request = camera->dequeueRequest();
        if (completed_request) {
            FrameBuffer *buffer = completed_request->buffers().at(streamConfig.stream());
            const MappedBuffer &mapped_buffer = completed_request->map(streamConfig.stream());

            save_raw_as_png(cfg.output_path, streamConfig.size.width, streamConfig.size.height,
                            streamConfig.stride, static_cast<const uint8_t*>(mapped_buffer.planes()[0].data));

            completed_request->reuse();
            captured = true;
        }
    }

    camera->stop();
    camera->release();
    cm->stop();
    return 0;
}
#else
#include <iostream>
int main(int argc, char** argv) {
    std::cerr << "This capture tool was built without libcamera support." << std::endl;
    std::cerr << "Please re-run cmake with -DUSE_LIBCAMERA=ON to enable it." << std::endl;
    return 1;
}
#endif


