#include "texture_utils.h"
#include <SDL.h> // Include the main SDL header to get SDL_GL_GetProcAddress
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h> // For the function pointer type definition
#include <vector>
#include <iostream>

void CreateOrUpdateTexture(uint32_t& texture_id, int width, int height, const std::vector<uint8_t>& pixel_data) {
    // Define the function pointer as a static variable. It will be initialized to nullptr once.
    static PFNGLGENERATEMIPMAPPROC p_glGenerateMipmap = nullptr;
    // A flag to ensure we only try to load the function pointer once.
    static bool loader_initialized = false;

    // Lazy-load the function pointer on the very first call to this function.
    if (!loader_initialized) {
        p_glGenerateMipmap = (PFNGLGENERATEMIPMAPPROC)SDL_GL_GetProcAddress("glGenerateMipmap");
        if (!p_glGenerateMipmap) {
            std::cerr << "Warning: glGenerateMipmap could not be loaded. Texture minification might be lower quality." << std::endl;
        }
        loader_initialized = true;
    }

    if (pixel_data.empty() || width <= 0 || height <= 0) return;

    if (texture_id == 0) {
        glGenTextures(1, &texture_id);
        glBindTexture(GL_TEXTURE_2D, texture_id);
        // Set up high-quality filtering with mipmaps
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); // Trilinear for minification
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);              // Bilinear for magnification
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, texture_id);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    // Upload base level of the texture
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, pixel_data.data());
    
    // Generate the mipmap chain for high-quality minification, ONLY if the pointer is valid.
    if (p_glGenerateMipmap) {
        p_glGenerateMipmap(GL_TEXTURE_2D);
    }
    
    // It's good practice to reset the alignment to the default value.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    
    glBindTexture(GL_TEXTURE_2D, 0);
}

void DeleteTexture(uint32_t& texture_id) {
    if (texture_id != 0) {
        glDeleteTextures(1, &texture_id);
        texture_id = 0;
    }
}
