#ifndef RAW_LOAD_H
#define RAW_LOAD_H

#include "HalideBuffer.h"
#include <string>
#include <memory>
#include <vector>
#include <cstdint>
#include "librawspeed/RawSpeed-API.h" // For allocator types

// A structure to hold all the essential data extracted from a RAW file.
struct RawImageData {
    Halide::Runtime::Buffer<uint16_t, 2> bayer_data;
    int cfa_pattern = 0;
    int black_level = 0;
    int white_level = 4095;
    bool has_matrix = false;
    float matrix_3200[3][4];
    float matrix_7000[3][4];

    // This smart pointer owns the raw file data buffer read by RawSpeed.
    // By keeping it here, we ensure the memory pointed to by bayer_data
    // remains valid for the lifetime of this struct.
    std::unique_ptr<std::vector<uint8_t, rawspeed::DefaultInitAllocatorAdaptor<uint8_t, rawspeed::AlignedAllocator<uint8_t, 16> > > > memory_owner;
};

// Loads a RAW file (e.g., DNG, ARW) using RawSpeed, extracts metadata,
// and returns the sanitized data in a RawImageData struct.
RawImageData load_raw(const std::string &path);

// Loads a 16-bit grayscale PNG (legacy format) and populates a
// RawImageData struct with default metadata.
RawImageData load_raw_png(const std::string &path);

#endif // RAW_LOAD_H
