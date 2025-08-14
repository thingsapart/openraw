#ifndef TEXTURE_UTILS_H
#define TEXTURE_UTILS_H

#include <cstdint>
#include <vector>
#include "HalideBuffer.h"

// Creates a new OpenGL texture or updates an existing one with data from an interleaved byte vector.
void CreateOrUpdateTexture(uint32_t& texture_id, int width, int height, const std::vector<uint8_t>& pixel_data);

// Deletes an OpenGL texture.
void DeleteTexture(uint32_t& texture_id);

#endif // TEXTURE_UTILS_H
