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


# === STAGE 1: The Generator ===
# The add_halide_generator function defines an executable target for our generator.
# The source file contains the logic from the official camera_pipe example.
add_halide_generator(pipeline_generator
    SOURCES src/pipeline_generator.cpp src/stage_demosaic.cpp
)


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

target_link_libraries(test_runner PRIVATE
    Halide::Halide
    Halide::ImageIO
)
add_test(NAME PipelineTests COMMAND test_runner)
