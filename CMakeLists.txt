cmake_minimum_required(VERSION 3.16)
project(CameraPipe)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Halide ---
# This looks for the Halide installation specified by -DHalide_DIR=...
# or a system-wide installation.
find_package(Halide REQUIRED)
message(STATUS "Found Halide: ${Halide_DIR}")

# Add a default target if not specified by the user/environment.
# This prevents the `target=` argument from being empty.
if(NOT CMAKE_HALIDE_TARGET)
  set(CMAKE_HALIDE_TARGET "host")
  message(STATUS "CMAKE_HALIDE_TARGET not set, defaulting to 'host'")
endif()

# --- Fetch STB Header-only Library ---
# This is required by tone_curve_utils.cpp to write PNG files.
include(FetchContent)
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
)
# This makes the content available and sets the stb_SOURCE_DIR variable.
FetchContent_MakeAvailable(stb)

# --- Build Options ---
option(USE_LIBCAMERA "Build the libcamera-based capture tool" OFF)
option(ENABLE_DENOISE "Enable the VST denoise stage" ON)


# ==============================================================================
#  1. PIPELINE GENERATOR
#     This target builds the tool that generates the static library for our
#     image processing pipeline.
# ==============================================================================
add_executable(pipeline_generator src/pipeline_generator.cpp)

# Add a compile definition to disable the denoise stage if the option is OFF.
if(NOT ENABLE_DENOISE)
  target_compile_definitions(pipeline_generator PRIVATE NO_DENOISE)
  message(STATUS "Denoise stage is DISABLED at compile time.")
else()
  message(STATUS "Denoise stage is ENABLED at compile time.")
endif()

# The generator needs to link against libHalide for the API, and against
# Halide::Generator to get the main() function that turns it into a
# runnable command-line tool.
target_link_libraries(pipeline_generator PRIVATE Halide::Halide Halide::Generator)

# ==============================================================================
#  2. STATIC PIPELINE LIBRARY
#     This custom command runs the generator we just built to produce the
#     actual pipeline code as a static library (e.g., camera_pipe_f32.a).
# ==============================================================================

# --- Define pipeline variants ---
# We create two versions of the pipeline, one for float and one for uint16.
set(PIPELINE_VARIANTS f32 u16)

foreach(VARIANT ${PIPELINE_VARIANTS})
    set(PIPELINE_NAME "camera_pipe_${VARIANT}")
    set(GENERATOR_NAME "camera_pipe_${VARIANT}")
    set(FILE_BASE_NAME "${PIPELINE_NAME}_lib")

    # This command runs the generator executable.
    # We pass the generator name, output directory, and target architecture.
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
            # Enable profiling hooks, but disable tracing hooks for now.
            profile=true
            trace=false
        # FIX: Add all source files that the generator depends on.
        # This ensures the generator is re-run whenever the pipeline definition changes.
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

    # This creates a "dummy" target for the generated library.
    add_custom_target(
        ${FILE_BASE_NAME} ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${FILE_BASE_NAME}.a
    )
endforeach()


# ==============================================================================
#  3. RUNNER EXECUTABLE
#     This is the main program that loads an image and runs it through the
#     pipeline. It needs to link against the generated static library.
# ==============================================================================
foreach(VARIANT ${PIPELINE_VARIANTS})
    set(PIPELINE_NAME "camera_pipe_${VARIANT}")
    set(FILE_BASE_NAME "${PIPELINE_NAME}_lib")
    set(PROCESS_TARGET "process_${VARIANT}")

    add_executable(${PROCESS_TARGET} src/process.cpp src/tone_curve_utils.cpp)

    # Add a compile definition to include the correct generated header.
    if(VARIANT STREQUAL "f32")
        target_compile_definitions(${PROCESS_TARGET} PRIVATE PIPELINE_PRECISION_F32)
    elseif(VARIANT STREQUAL "u16")
        target_compile_definitions(${PROCESS_TARGET} PRIVATE PIPELINE_PRECISION_U16)
    endif()

    # The runner needs to know where to find the generated header file
    # from this build AND the stb header files from FetchContent.
    target_include_directories(${PROCESS_TARGET} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${stb_SOURCE_DIR}
    )

    # Link the runner against the generated pipeline library.
    target_link_libraries(${PROCESS_TARGET} PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/${FILE_BASE_NAME}.a
    )

    # Link against Halide runtime, ImageIO, and other system libraries.
    target_link_libraries(${PROCESS_TARGET} PRIVATE
        Halide::Runtime
        Halide::ImageIO
        ${CMAKE_DL_LIBS}
        ${CMAKE_THREAD_LIBS_INIT}
    )
endforeach()


# ==============================================================================
#  4. CAPTURE TOOL (Optional)
# ==============================================================================
if(USE_LIBCAMERA)
    find_package(libcamera REQUIRED)
    find_package(libpng REQUIRED)

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
