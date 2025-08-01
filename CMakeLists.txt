# CMakeLists.txt for a Halide Camera Pipeline based on the official example.

cmake_minimum_required(VERSION 3.16)
project(HalideCameraPipe CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies ---
find_package(Halide REQUIRED)
message(STATUS "Found Halide: ${Halide_VERSION}")

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
    SOURCES src/pipeline_generator.cpp
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
add_executable(process src/process.cpp)

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
