cmake_minimum_required(VERSION 3.16)
project(CameraPipe)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Fetch Dependencies ---
# Bring in the stb library for simple image writing.
include(FetchContent)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master 
)
FetchContent_MakeAvailable(stb)


# --- Find Halide ---
# This is the standard way to find a Halide installation.
find_package(Halide REQUIRED)
message(STATUS "Found Halide: ${Halide_VERSION}")
message(STATUS "Found Halide library: ${Halide_LIBRARY}")

# --- Set up the Pipeline Generator Executable ---
# Use add_halide_generator to create the executable that can run our generators.
add_halide_generator(pipeline_generator
    SOURCES src/pipeline_generator.cpp)

# --- Generate the Static Libraries from the Generator Executable ---
# We no longer need the 'camera_pipe' alias.

# Target 1: The explicit 'camera_pipe_f32' for the float32 pipeline
add_halide_library(camera_pipe_f32_lib
    FROM pipeline_generator
    GENERATOR camera_pipe_f32
    FUNCTION_NAME camera_pipe_f32)

# Target 2: The explicit 'camera_pipe_u16' for the uint16 pipeline
add_halide_library(camera_pipe_u16_lib
    FROM pipeline_generator
    GENERATOR camera_pipe_u16
    FUNCTION_NAME camera_pipe_u16)


# We do the same for the auto-scheduled versions if they exist.
set(AUTOSCHEDULE_PLUGIN ${Halide_GEN_AUTOSCHEDULE_PLUGIN})
if(AUTOSCHEDULE_PLUGIN)
    message(STATUS "Found Halide auto-scheduler: ${AUTOSCHEDULE_PLUGIN}")
    add_halide_library(camera_pipe_f32_auto_schedule_lib
        FROM pipeline_generator
        GENERATOR camera_pipe_f32
        FUNCTION_NAME camera_pipe_f32_auto_schedule
        AUTOSCHEDULE ${AUTOSCHEDULE_PLUGIN})

    add_halide_library(camera_pipe_u16_auto_schedule_lib
        FROM pipeline_generator
        GENERATOR camera_pipe_u16
        FUNCTION_NAME camera_pipe_u16_auto_schedule
        AUTOSCHEDULE ${AUTOSCHEDULE_PLUGIN})
endif()


# --- Build the Runners (process_f32 and process_u16) ---
# Find the dependencies required by Halide::Tools for image I/O.
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)

# --- Runner for the f32 pipeline ---
add_executable(process_f32 src/process.cpp src/tone_curve_utils.cpp)
# Define a macro so process.cpp knows which pipeline to use
target_compile_definitions(process_f32 PRIVATE PIPELINE_PRECISION_F32)
target_include_directories(process_f32 PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(process_f32 PRIVATE
    Halide::Runtime
    Halide::Tools
    camera_pipe_f32_lib
    PNG::PNG
    JPEG::JPEG)
if(AUTOSCHEDULE_PLUGIN)
    target_link_libraries(process_f32 PRIVATE camera_pipe_f32_auto_schedule_lib)
endif()

# --- Runner for the u16 pipeline ---
add_executable(process_u16 src/process.cpp src/tone_curve_utils.cpp)
# Define a macro so process.cpp knows which pipeline to use
target_compile_definitions(process_u16 PRIVATE PIPELINE_PRECISION_U16)
target_include_directories(process_u16 PRIVATE ${stb_SOURCE_DIR})
target_link_libraries(process_u16 PRIVATE
    Halide::Runtime
    Halide::Tools
    camera_pipe_u16_lib
    PNG::PNG
    JPEG::JPEG)
if(AUTOSCHEDULE_PLUGIN)
    target_link_libraries(process_u16 PRIVATE camera_pipe_u16_auto_schedule_lib)
endif()


# --- Build the Capture Tool (Optional) ---
option(USE_LIBCAMERA "Build the libcamera-based capture tool" OFF)

if(USE_LIBCAMERA)
    find_package(libcamera REQUIRED)
    find_package(libpng REQUIRED)
    add_executable(capture src/capture.cpp)
    # Add libcamera include directories
    target_include_directories(capture PRIVATE ${LIBCAMERA_INCLUDE_DIRS})
    target_link_libraries(capture PRIVATE ${LIBCAMERA_LIBRARIES} ${LIBPNG_LIBRARIES})
    # Add a definition so the code knows libcamera is available
    target_compile_definitions(capture PRIVATE USE_LIBCAMERA)
    message(STATUS "Building capture tool with libcamera support.")
else()
    # Create a dummy capture target if libcamera is not used
    add_executable(capture src/capture.cpp)
    message(STATUS "Building capture tool stub (libcamera support is OFF).")
endif()
