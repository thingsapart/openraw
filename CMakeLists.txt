cmake_minimum_required(VERSION 3.16)
project(RawrPipe)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set a default build type if none is specified. This is good practice and
# required by the RawSpeed build scripts.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# --- Find Core Dependencies ---
find_package(Halide REQUIRED)
message(STATUS "Found Halide: ${Halide_DIR}")
find_package(PNG REQUIRED)
find_package(PkgConfig REQUIRED)

# --- Fetch Dependencies ---
include(FetchContent)
# Do NOT set FETCHCONTENT_BASE_DIR to a source directory. Let CMake use its default
# location inside the build tree. This is the key fix for the "write to source" error.

FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
  GIT_SHALLOW    TRUE
)

FetchContent_Declare(
  rawspeed
  GIT_REPOSITORY https://github.com/darktable-org/rawspeed.git
  GIT_TAG        develop
  GIT_SHALLOW    TRUE
)

# Disable building tests and OpenMP for the RawSpeed subproject before making it available.
set(BUILD_TESTING OFF)
set(BUILD_BENCHMARKING OFF)
set(WITH_OPENMP ON)
# Use the modern, simpler MakeAvailable call for both.
FetchContent_MakeAvailable(stb rawspeed)


# Add a default target if not specified by the user/environment.
if(NOT CMAKE_HALIDE_TARGET)
  set(CMAKE_HALIDE_TARGET "host")
  message(STATUS "CMAKE_HALIDE_TARGET not set, defaulting to 'host'")
endif()

option(USE_LIBCAMERA "Build the libcamera-based capture tool" OFF)
option(USE_LENSFUN "Use Lensfun lens correction tool" ON)
if(USE_LENSFUN)
    pkg_check_modules(LENSFUN REQUIRED lensfun)
    if(LENSFUN_FOUND)
        message(STATUS "Found Lensfun (via pkg-config)")
        add_definitions(-DUSE_LENSFUN)
    else()
        message(WARNING "Lensfun library (lensfun-1) not found by pkg-config. Building without lens correction support.")
        set(USE_LENSFUN OFF)
    endif()
endif()

# ==============================================================================
#  1. PIPELINE GENERATOR
# ==============================================================================
add_executable(pipeline_generator src/pipeline_generator.cpp src/color_tools.cpp src/tone_curve_utils.cpp)
target_link_libraries(pipeline_generator PRIVATE Halide::Halide Halide::Generator)
target_include_directories(pipeline_generator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${stb_SOURCE_DIR})

# ==============================================================================
#  2. STATIC PIPELINE LIBRARY GENERATION
# ==============================================================================
set(PIPELINE_VARIANTS f32 u16)
set(GENERATED_PIPELINE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated_pipeline)
file(MAKE_DIRECTORY ${GENERATED_PIPELINE_DIR})

foreach(VARIANT ${PIPELINE_VARIANTS})
    set(PIPELINE_NAME "camera_pipe_${VARIANT}")
    set(FILE_BASE_NAME "${PIPELINE_NAME}_lib")

    add_custom_command(
        OUTPUT
            ${GENERATED_PIPELINE_DIR}/${FILE_BASE_NAME}.a
            ${GENERATED_PIPELINE_DIR}/${FILE_BASE_NAME}.h
        COMMAND
            $<TARGET_FILE:pipeline_generator>
            -g ${PIPELINE_NAME}
            -o ${GENERATED_PIPELINE_DIR}
            -n ${FILE_BASE_NAME}
            -e "static_library,c_header"
            target=${CMAKE_HALIDE_TARGET}
            profile=false
        DEPENDS
            pipeline_generator src/pipeline_generator.cpp src/pipeline_schedule.h
            src/demosaic_AHD.h src/demosaic_fast.h src/demosaic_LMMSE.h src/demosaic_RI.h
            src/denoise_guided.h src/denoise_nlmeans.h src/halide_guided_filter.h
            src/pipeline_helpers.h src/stage_apply_curve.h src/stage_ca_correct.h
            src/stage_color_correct.h src/stage_deinterleave.h src/stage_demosaic.h
            src/stage_denoise.h src/stage_exposure.h src/stage_hot_pixel_suppression.h
            src/stage_local_adjust_bilateral.h src/stage_local_adjust_laplacian.h
            src/stage_local_tonal_adjustments.h src/stage_normalize_and_expose.h
            src/stage_resize.h src/stage_saturation.h src/stage_sharpen.h
            src/tone_curve_utils.h src/process_options.h src/stage_bayer_normalize.h
        COMMENT "Generating ${FILE_BASE_NAME} library..."
    )
    add_custom_target(
        generate_${PIPELINE_NAME} ALL
        DEPENDS ${GENERATED_PIPELINE_DIR}/${FILE_BASE_NAME}.a
    )
endforeach()


# ==============================================================================
#  3. RUNNER EXECUTABLE (`process`)
# ==============================================================================
foreach(VARIANT ${PIPELINE_VARIANTS})
    set(PIPELINE_NAME "camera_pipe_${VARIANT}")
    set(FILE_BASE_NAME "${PIPELINE_NAME}_lib")
    set(PROCESS_TARGET "process_${VARIANT}")

    add_executable(${PROCESS_TARGET}
                  src/process.cpp
                  src/tone_curve_utils.cpp
                  src/process_options.cpp
                  src/color_tools.cpp
                  src/tone_curve_utils.cpp
                  src/raw_load.cpp
                )

    if(VARIANT STREQUAL "f32")
        target_compile_definitions(${PROCESS_TARGET} PRIVATE PIPELINE_PRECISION_F32)
    elseif(VARIANT STREQUAL "u16")
        target_compile_definitions(${PROCESS_TARGET} PRIVATE PIPELINE_PRECISION_U16)
    endif()

    target_include_directories(${PROCESS_TARGET} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${GENERATED_PIPELINE_DIR}
        ${stb_SOURCE_DIR}
        ${rawspeed_SOURCE_DIR}/src
    )
    target_link_directories(${PROCESS_TARGET} PRIVATE ${LENSFUN_LIBRARY_DIRS})
    target_link_libraries(${PROCESS_TARGET} PRIVATE
        ${GENERATED_PIPELINE_DIR}/${FILE_BASE_NAME}.a
        rawspeed
        Halide::Runtime
        Halide::ImageIO
        PNG::PNG
        ${CMAKE_DL_LIBS}
        ${LENSFUN_LIBRARIES}
    )
    add_dependencies(${PROCESS_TARGET} generate_${PIPELINE_NAME})
endforeach()


# ==============================================================================
#  4. CAPTURE TOOL
# ==============================================================================
option(USE_LIBCAMERA "Build the libcamera-based capture tool" OFF)
if(USE_LIBCAMERA)
    find_package(libcamera REQUIRED)
    add_executable(capture src/capture.cpp)
    target_link_libraries(capture PRIVATE libcamera::libcamera PNG::PNG)
    message(STATUS "Capture tool will be built.")
else()
    add_executable(capture src/capture.cpp)
    message(STATUS "Capture tool is disabled. Use -DUSE_LIBCAMERA=ON to build it.")
endif()


# ==============================================================================
#  5. NEW RAW Editor UI Application (`rawr`)
# ==============================================================================
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
message(STATUS "Found SDL2: ${SDL2_LIBRARY}")
message(STATUS "Found OpenGL: ${OPENGL_gl_LIBRARY}")

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy
                  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/imgui.CMakeLists.txt
                  <SOURCE_DIR>/CMakeLists.txt
)
FetchContent_MakeAvailable(imgui)
target_link_libraries(imgui PRIVATE SDL2::SDL2)

# --- Download font and copy it to the build directory ---
set(FONT_DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/font_download)
set(FONT_TTF_PATH ${FONT_DOWNLOAD_DIR}/extras/ttf/Inter-Regular.ttf)
set(FONT_DESTINATION_DIR ${CMAKE_BINARY_DIR})

if(NOT EXISTS ${FONT_TTF_PATH})
    message(STATUS "Inter font not found. Downloading...")
    set(FONT_ZIP_PATH ${FONT_DOWNLOAD_DIR}/Inter.zip)
    file(DOWNLOAD https://github.com/rsms/inter/releases/download/v4.0/Inter-4.0.zip
         ${FONT_ZIP_PATH}
         SHOW_PROGRESS
         STATUS download_status)
    if(NOT download_status EQUAL 0)
        message(FATAL_ERROR "Failed to download Inter font zip file.")
    endif()
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf ${FONT_ZIP_PATH}
        WORKING_DIRECTORY ${FONT_DOWNLOAD_DIR}
    )
endif()
file(COPY ${FONT_TTF_PATH} DESTINATION ${FONT_DESTINATION_DIR})

add_executable(rawr
    src/editor/main.cpp
    src/editor/editor_ui.cpp
    src/editor/editor_theme.cpp
    src/editor/halide_runner.cpp
    src/editor/texture_utils.cpp
    src/editor/texture_utils.cpp
    src/editor/curves_editor.cpp
    src/process_options.cpp
    src/tone_curve_utils.cpp
    src/color_tools.cpp
    src/raw_load.cpp
    src/tone_curve_utils.cpp
    src/editor/pane_manager.cpp
    src/editor/imgui_custom_widgets.cpp
    src/editor/panes/pane_color_curves.cpp
    src/editor/panes/pane_color_wheels.cpp
    src/editor/panes/pane_core_pipeline.cpp
    src/editor/panes/pane_denoise.cpp
    src/editor/panes/pane_histogram_curves.cpp
    src/editor/panes/pane_local_adjustments.cpp
    src/editor/panes/pane_preview.cpp
    src/editor/panes/pane_tone_curves.cpp
    src/editor/panes/pane_lens_correction.cpp
    src/editor/panes/pane_dehaze.cpp
    src/editor/panes/pane_lens_correction.cpp
)

target_compile_definitions(rawr PRIVATE USE_LENSFUN)
target_link_directories(rawr PRIVATE ${LENSFUN_LIBRARY_DIRS})
target_link_libraries(rawr PRIVATE
    imgui
    OpenGL::GL
    Halide::ImageIO
    PNG::PNG
    ${GENERATED_PIPELINE_DIR}/camera_pipe_f32_lib.a
    rawspeed
    Halide::Runtime
    ${LENSFUN_LIBRARIES}
)
add_dependencies(rawr generate_camera_pipe_f32)

target_include_directories(rawr PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/editor
    ${stb_SOURCE_DIR}
    ${GENERATED_PIPELINE_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${rawspeed_SOURCE_DIR}/src
)
set_target_properties(rawr PROPERTIES MACOSX_RPATH ON)

if(LENSFUN_FOUND)
    # --- Build the lens_info utility ---
    add_executable(lens_info src/lens_info.cpp)
    target_include_directories(lens_info PRIVATE ${LENSFUN_INCLUDE_DIRS})
    target_link_directories(lens_info PRIVATE ${LENSFUN_LIBRARY_DIRS})
    target_link_libraries(lens_info PRIVATE ${LENSFUN_LIBRARIES})
endif()
