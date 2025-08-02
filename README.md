# Halide RAW Processing Pipeline (CameraPipe Base)

This project is a high-quality RAW-to-RGB image processing pipeline based on the official `camera_pipe` example from the Halide distribution. It serves as a robust and well-structured foundation for a custom camera system.

The system is composed of three main parts:
1.  **Capture Tool (`src/capture.cpp`):** A stand-alone program for Raspberry Pi that uses `libcamera` to capture high-quality RAW images and save them as 16-bit PNG files. This is the recommended way to generate input for the pipeline.
2.  **Pipeline Generator (`src/pipeline_generator.cpp`):** Contains the Halide code that defines the multi-stage image processing graph. This is compiled into a tool that generates the static libraries for the pipeline.
3.  **Runner (`src/process.cpp`):** A command-line application that loads a 16-bit RAW PNG, processes it through the compiled Halide pipeline, and saves the result as a standard 8-bit image.

## Build Requirements

- A C++17 compliant compiler (g++ or clang)
- `cmake` (version 3.16 or newer)
- The Halide library distribution.
- For the Capture Tool: `libcamera-dev` and `libpng-dev` (or equivalent).

## How to Build

1.  **Configure CMake.** From the project root, create a build directory. You must point CMake to your Halide installation.

    ```bash
    # For a local Halide download/build
    cmake -S . -B build -DHalide_DIR=/path/to/your/halide/lib/cmake/Halide

    # For a system-wide install (e.g., via Homebrew)
    cmake -S . -B build
    ```

2.  **Enable the Capture Tool (Optional, on Raspberry Pi).**
    If you want to build the `capture` utility, add the `-DUSE_LIBCAMERA=ON` flag:

    ```bash
    cmake -S . -B build -DHalide_DIR=... -DUSE_LIBCAMERA=ON
    ```

3.  **Build the project.**

    ```bash
    cmake --build build
    ```

## How to Run

### Step 1: Capture a RAW Image (on Raspberry Pi)

Use the `capture` tool to create a 16-bit RAW PNG file.

```bash
# Capture with auto-exposure and save it
./build/capture raw_image.png

# Capture with a manual exposure of 1/100s (10000 us)
./build/capture --exposure 10000 raw_manual.png
```

### Step 2: Process the RAW Image

Use the `process` tool to run the pipeline. Arguments are specified with flags. Run with `--help` for a full list of options.

**Usage:** `./process --input <path> --output <path> [options...]`

```bash
# Example using a global custom tone curve to crush blacks and lift mid-tones
./build/process --input raw_image.png --output processed_curve.png \
    --curve-points "0.1:0,0.5:0.6,1:1"

# Example using per-channel curves for a "cross-processed" look
# - Crushes blacks and lifts whites in the Red channel
# - Standard S-curve in the Green channel
# - Lifts blacks and darkens highlights in the Blue channel
./build/process --input raw_image.png --output processed_cross.png \
    --curve-r "0.1:0,0.9:1" \
    --curve-b "0:0.1,1:0.9"

# See all available options
./build/process --help
```
The program will print benchmark timings for both the manually-scheduled and auto-scheduled versions of the pipeline.

