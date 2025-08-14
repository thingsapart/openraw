cmake_minimum_required(VERSION 3.16)
project(CameraPipe C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- Add custom module path and Homebrew prefix ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
if(APPLE AND EXISTS "/opt/homebrew")
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

# --- Find Halide ---
find_package(Halide REQUIRED)
message(STATUS "Found Halide: ${Halide_DIR}")

# Add a default target if not specified by the user/environment.
if(NOT CMAKE_HALIDE_TARGET)
  set(CMAKE_HALIDE_TARGET "host")
  message(STATUS "CMAKE_HALIDE_TARGET not set, defaulting to 'host'")
endif()

# --- Fetch STB Header-only Library ---
include(FetchContent)
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

# --- Build Options ---
option(USE_LIBCAMERA "Build the libcamera-based capture tool" OFF)
option(BUILD_LVGL_EDITOR "Build the LVGL-based editor executable" OFF)

# --- Core Dependencies ---
# PNG is used by both the pipeline and the optional editor
find_package(PNG REQUIRED)


# ==============================================================================
#  1. PIPELINE GENERATOR (Original, restored from diff)
# ==============================================================================
add_executable(pipeline_generator src/pipeline_generator.cpp)
target_link_libraries(pipeline_generator PRIVATE Halide::Halide Halide::Generator)


# ==============================================================================
#  2. STATIC PIPELINE LIBRARY (Original, restored from diff)
# ==============================================================================
set(PIPELINE_VARIANTS f32 u16)
foreach(VARIANT ${PIPELINE_VARIANTS})
    set(PIPELINE_NAME "camera_pipe_${VARIANT}")
    set(GENERATOR_NAME "camera_pipe_${VARIANT}")
    set(FILE_BASE_NAME "${PIPELINE_NAME}_lib")

    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/${FILE_BASE_NAME}.a
            ${CMAKE_CURRENT_BINARY_DIR}/${FILE_BASE_NAME}.h
        COMMAND
            $<TARGET_FILE:pipeline_generator>
            -g ${GENERATOR_NAME}
            -o ${CMAKE_CURRENT_BINARY_DIR}
            -n ${FILE_BASE_NAME}
            -e "static_library,c_header"
            target=${CMAKE_HALIDE_TARGET}
            profile=true
            trace=false
        DEPENDS
            pipeline_generator
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline_generator.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stage_denoise.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stage_ca_correct.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stage_deinterleave.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stage_demosaic.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stage_color_correct.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stage_apply_curve.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/stage_sharpen.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/halide_guided_filter.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline_helpers.h
        COMMENT
            "Generating ${FILE_BASE_NAME} static library..."
    )
    add_custom_target(
        ${FILE_BASE_NAME} ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${FILE_BASE_NAME}.a
    )
endforeach()


# ==============================================================================
#  3. RUNNER EXECUTABLE (Original, restored from diff)
# ==============================================================================
foreach(VARIANT ${PIPELINE_VARIANTS})
    set(PIPELINE_NAME "camera_pipe_${VARIANT}")
    set(FILE_BASE_NAME "${PIPELINE_NAME}_lib")
    set(PROCESS_TARGET "process_${VARIANT}")

    add_executable(${PROCESS_TARGET} src/process.cpp src/tone_curve_utils.cpp)

    if(VARIANT STREQUAL "f32")
        target_compile_definitions(${PROCESS_TARGET} PRIVATE PIPELINE_PRECISION_F32)
    elseif(VARIANT STREQUAL "u16")
        target_compile_definitions(${PROCESS_TARGET} PRIVATE PIPELINE_PRECISION_U16)
    endif()

    target_include_directories(${PROCESS_TARGET} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${stb_SOURCE_DIR}
    )
    target_link_libraries(${PROCESS_TARGET} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/${FILE_BASE_NAME}.a
    )
    target_link_libraries(${PROCESS_TARGET} PRIVATE
        Halide::Runtime
        Halide::ImageIO
        ${CMAKE_DL_LIBS}
        ${CMAKE_THREAD_LIBS_INIT}
    )
endforeach()


# ==============================================================================
#  4. CAPTURE TOOL (Original, restored from diff)
# ==============================================================================
if(USE_LIBCAMERA)
    find_package(libcamera REQUIRED)
    add_executable(capture src/capture.cpp)
    target_link_libraries(capture PRIVATE
        libcamera::libcamera
        PNG::PNG
    )
    message(STATUS "Capture tool will be built.")
else()
    add_executable(capture src/capture.cpp)
    message(STATUS "Capture tool is disabled. Use -DUSE_LIBCAMERA=ON to build it.")
endif()


# ===================================================================
# Optional LVGL Editor Target
# ===================================================================

if(BUILD_LVGL_EDITOR)
    message(STATUS "Building optional LVGL editor target.")

    # Find editor-specific dependencies only when the option is enabled.
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)

    # Add LVGL v9.x as a library, built from the 'lvgl' subdirectory.
    file(GLOB_RECURSE LVGL_SOURCES
        "lvgl/src/*.c"
        "lvgl/src/drivers/sdl/*.c"
    )
    add_library(lvgl STATIC ${LVGL_SOURCES})
    target_link_libraries(lvgl PRIVATE SDL2::SDL2 PNG::PNG)
    target_compile_definitions(lvgl PRIVATE
        LV_CONF_PATH=<editor/lv_conf.h>
        LV_BUILD_CONF_PATH=<editor/lv_conf.h>
    )
    target_include_directories(lvgl PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/lvgl"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/editor"
    )

    # Create a helper library from the refactored 'editor' directory.
    add_library(lvgl_helpers STATIC
        src/editor/sdl_viewer.c
        src/editor/lvgl_assert_handler.c
    )
    target_link_libraries(lvgl_helpers PUBLIC
        lvgl
        SDL2::SDL2
        SDL2::Image
        m
        PNG::PNG
    )
    target_include_directories(lvgl_helpers PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src/editor"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_definitions(lvgl_helpers PUBLIC
        LV_CONF_PATH=<editor/lv_conf.h>
        LV_BUILD_CONF_PATH=<editor/lv_conf.h>
    )

    # Define the 'editor' executable.
    add_executable(editor src/editor/main.c)
    target_link_libraries(editor PRIVATE lvgl_helpers)
    target_compile_definitions(editor PRIVATE
        _GNU_SOURCE
        __DEV_MODE__
    )

    # Use target_compile_options to apply flags correctly.
    target_compile_options(lvgl PRIVATE -Wall -g)
    target_compile_options(lvgl_helpers PRIVATE -Wall -g)
    target_compile_options(editor PRIVATE -Wall -g)

endif()
