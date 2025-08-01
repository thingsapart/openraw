# CMakeLists.txt for the Halide Camera Pipeline project (Modern Idiomatic Version)

cmake_minimum_required(VERSION 3.16)
project(HalideCameraPipe CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Find Dependencies ---
find_package(Halide REQUIRED)
message(STATUS "Found Halide: ${Halide_VERSION}")

# Define an option to enable/disable camera support
option(USE_LIBCAMERA "Build with libcamera support for the capture tool" OFF)

if(USE_LIBCAMERA)
    find_package(Libcamera REQUIRED)
    find_package(PNG REQUIRED)
    message(STATUS "Found Libcamera: ${Libcamera_VERSION}")
    message(STATUS "Found LibPNG: ${PNG_VERSION}")
endif()

# --- Download stb headers ---
include(FetchContent)
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(stb)


# === STAGE 1: The Generator ===
add_halide_generator(pipeline_generator
    SOURCES src/pipeline_generator.cpp
)


# === STAGE 2: Generate the Pipeline Libraries ===
# 1. The manually-scheduled RAW pipeline.
add_halide_library(camera_pipe
    FROM pipeline_generator
    GENERATOR camera_pipe
)

# 2. The auto-scheduled RAW pipeline.
add_halide_library(camera_pipe_auto_schedule
    FROM pipeline_generator
    GENERATOR camera_pipe
    AUTOSCHEDULER Halide::Mullapudi2016
)

# 3. The manually-scheduled RGB pipeline.
add_halide_library(rgb_pipe
    FROM pipeline_generator
    GENERATOR rgb_pipe
)

# 4. The auto-scheduled RGB pipeline.
add_halide_library(rgb_pipe_auto_schedule
    FROM pipeline_generator
    GENERATOR rgb_pipe
    AUTOSCHEDULER Halide::Mullapudi2016
)


# === STAGE 3: The Runner (Main Processor) ===
add_executable(process src/process.cpp)

target_include_directories(process PRIVATE
    ${stb_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(process PRIVATE
    camera_pipe
    camera_pipe_auto_schedule
    rgb_pipe
    rgb_pipe_auto_schedule
)

# Add a define to the runner if the auto-scheduled targets don't exist.
if (NOT TARGET camera_pipe_auto_schedule OR NOT TARGET rgb_pipe_auto_schedule)
    target_compile_definitions(process PRIVATE NO_AUTO_SCHEDULE)
endif()


# === STAGE 4: The Capture Tool (Optional) ===
if(USE_LIBCAMERA)
    add_executable(capture src/capture.cpp)
    target_link_libraries(capture PRIVATE
        Libcamera::libcamera
        PNG::PNG
    )
endif()


# === STAGE 5: Testing ===
# Use a standard RGB image for testing the main processing path.
set(TEST_IMAGE ${CMAKE_CURRENT_SOURCE_DIR}/rgb.png)
if (EXISTS ${TEST_IMAGE})
    configure_file(${TEST_IMAGE} ${CMAKE_CURRENT_BINARY_DIR}/rgb.png COPYONLY)
    add_test(NAME process_rgb_test_bypass
             COMMAND process --input-type rgb --bypass-demosaic --input rgb.png --output test_out_bypass.png)
    set_tests_properties(process_rgb_test_bypass PROPERTIES
                         PASS_REGULAR_EXPRESSION "Success!"
                         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif ()
