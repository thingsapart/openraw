# CMakeLists.txt for a Halide Camera Pipeline based on the official example.

cmake_minimum_required(VERSION 3.16)
project(HalideCameraPipe CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies ---
find_package(Halide REQUIRED)
message(STATUS "Found Halide: ${Halide_VERSION}")

# --- External Dependencies (stb) ---
# Use FetchContent to automatically download stb_image_write.h at configure time.
include(FetchContent)
FetchContent_Declare(
    stb_image_write
    URL https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
    URL_HASH SHA256=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05
    DOWNLOAD_NO_EXTRACT TRUE       # This is a single file, not an archive
)
FetchContent_MakeAvailable(stb_image_write)


# --- Libcamera Support ---
option(USE_LIBCAMERA "Build with libcamera support for the capture tool" OFF)
if(USE_LIBCAMERA)
    find_package(Libcamera REQUIRED)
    find_package(PNG REQUIRED)
endif()

# --- Configurable Pipeline Stages ---
# Add options to disable individual stages via preprocessor defines.
option(DISABLE_HOT_PIXEL_SUPPRESSION "Bypass Hot Pixel Suppression" OFF)
option(DISABLE_CA_CORRECT "Bypass Chromatic Aberration Correction" OFF)
option(DISABLE_DEINTERLEAVE "Bypass Deinterleaving" OFF)
option(DISABLE_COLOR_CORRECT "Bypass Color Correction" OFF)
option(DISABLE_SATURATION "Bypass Saturation Adjustment" OFF)
option(DISABLE_APPLY_CURVE "Bypass Tone Curve Application" OFF)
option(DISABLE_SHARPEN "Bypass Sharpening" OFF)

# Collect the chosen defines to pass to the compiler.
set(PIPELINE_DEFINES)
if(DISABLE_HOT_PIPELI_SUPPRESSION)
    list(APPEND PIPELINE_DEFINES "NO_HOT_PIXEL_SUPPRESSION")
endif()
if(DISABLE_CA_CORRECT)
    list(APPEND PIPELINE_DEFINES "NO_CA_CORRECT")
endif()
if(DISABLE_DEINTERLEAVE)
    list(APPEND PIPELINE_DEFINES "NO_DEINTERLEAVE")
endif()
if(DISABLE_COLOR_CORRECT)
    list(APPEND PIPELINE_DEFINES "NO_COLOR_CORRECT")
endif()
if(DISABLE_SATURATION)
    list(APPEND PIPELINE_DEFINES "NO_SATURATION")
endif()
if(DISABLE_APPLY_CURVE)
    list(APPEND PIPELINE_DEFINES "NO_APPLY_CURVE")
endif()
if(DISABLE_SHARPEN)
    list(APPEND PIPELINE_DEFINES "NO_SHARPEN")
endif()


# === STAGE 1: The Generator ===
# The add_halide_generator function defines an executable target for our generator.
# The source file contains the logic from the official camera_pipe example.
add_halide_generator(pipeline_generator
    SOURCES src/pipeline_generator.cpp src/stage_demosaic.cpp
)
# Pass the stage-disable defines to the generator
target_compile_definitions(pipeline_generator PRIVATE ${PIPELINE_DEFINES})


# === STAGE 2: Generate the Pipeline Libraries ===
# This is the canonical pattern. We create two separate library targets
# from the same generator: one manually scheduled, one auto-scheduled.

# 1. The manually-scheduled version.
add_halide_library(camera_pipe
    FROM pipeline_generator
    GENERATOR camera_pipe  # The name registered with HALIDE_REGISTER_GENERATOR
)

# 2. The auto-scheduled version.
add_halide_library(camera_pipe_auto_schedule
    FROM pipeline_generator
    GENERATOR camera_pipe
    AUTOSCHEDULER Halide::Li2018
)


# === STAGE 3: The Runner (Main Processor) ===
add_executable(process src/process.cpp src/tone_curve_utils.cpp)

# Add the include directory for the downloaded stb header.
# FetchContent makes the path available via the <name>_SOURCE_DIR variable.
target_include_directories(process PRIVATE ${stb_image_write_SOURCE_DIR})

# The runner from the example requires Halide's ImageIO and Tools libraries.
# Linking against the generated library targets automatically handles dependencies,
# include paths for the generated headers, and the Halide runtime.
target_link_libraries(process PRIVATE
    camera_pipe
    camera_pipe_auto_schedule
    Halide::ImageIO  # For load_and_convert_image
    Halide::Tools    # For halide_benchmark.h
)

# Add a define to the runner if the auto-scheduled target doesn't exist.
if (NOT TARGET camera_pipe_auto_schedule)
    target_compile_definitions(process PRIVATE NO_AUTO_SCHEDULE)
endif()


# === STAGE 4: The Capture Tool (Optional) ===
if(USE_LIBCAMERA)
    add_executable(capture src/capture.cpp)
    # The capture tool has its own dependencies.
    target_include_directories(capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(capture PRIVATE
        Libcamera::libcamera
        PNG::PNG
    )
endif()


# === STAGE 5: The Test Runner ===
# Use file(GLOB) to find all test source files automatically.
file(GLOB TEST_SOURCES "src/tests/*.cpp")
add_executable(test_runner ${TEST_SOURCES} src/tone_curve_utils.cpp src/stage_demosaic.cpp)

# The test runner needs include paths for the main src/ dir and stb.
target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${stb_image_write_SOURCE_DIR}
)
# Pass the stage-disable defines to the test runner as well
target_compile_definitions(test_runner PRIVATE ${PIPELINE_DEFINES})

target_link_libraries(test_runner PRIVATE
    Halide::Halide
    Halide::ImageIO
)
add_test(NAME PipelineTests COMMAND test_runner)
