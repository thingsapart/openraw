#!/bin/bash
# Note: This script is now compatible with Bash 3.x (default on macOS) and newer versions.
set -e # Exit immediately if a command exits with a non-zero status.

# --- Configuration ---
BUILD_DIR="build"
RUN_SCRIPT="./run.sh"

# List of CMake options to toggle. These correspond to the options
# added in CMakeLists.txt.
STAGES=(
    "HOT_PIXEL_SUPPRESSION"
    "CA_CORRECT"
    "DEINTERLEAVE"
    "COLOR_CORRECT"
    "SATURATION"
    "APPLY_CURVE"
    "SHARPEN"
)

# --- Sanity Checks ---
if [ ! -d "$BUILD_DIR" ] || [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "Error: Build directory '$BUILD_DIR' not found or not configured." >&2
    echo "Please run 'cmake -S . -B $BUILD_DIR -DHalide_DIR=...' at least once before running this script." >&2
    exit 1
fi

if [ ! -f "$RUN_SCRIPT" ]; then
    echo "Error: Benchmark runner script '$RUN_SCRIPT' not found." >&2
    exit 1
fi

# --- Helper Function to Run a Single Benchmark ---
# Arguments:
#   $1: The CMake flag to set (e.g., "-DDISABLE_SHARPEN=ON")
#
# This function prints informational text to stderr and the final
# numeric result to stdout, so it can be captured by command substitution.
run_benchmark() {
    local CMAKE_FLAG=$1
    echo >&2 # Print a blank line to stderr for spacing
    if [ -z "$CMAKE_FLAG" ]; then
        echo "--- Building with all stages ENABLED (Baseline) ---" >&2
    else
        echo "--- Building with flag: $CMAKE_FLAG ---" >&2
    fi

    # 1. Re-configure CMake with the new flag. Redirect verbose output.
    cmake -B "$BUILD_DIR" $ALL_CMAKE_FLAGS $CMAKE_FLAG > /dev/null

    # 2. Build the project. Redirect verbose output.
    echo "Building project..." >&2
    # Ensure -j$(nproc) works on macOS. macOS uses `sysctl -n hw.ncpu` for core count.
    BUILD_JOBS=$( (which nproc > /dev/null && nproc) || sysctl -n hw.ncpu )
    cmake --build "$BUILD_DIR" -- -j$BUILD_JOBS > /dev/null

    # 3. Run the benchmark and capture the output.
    #    Redirect stderr to stdout (2>&1) to capture all output.
    echo "Running benchmark..." >&2
    local OUTPUT
    OUTPUT=$($RUN_SCRIPT 2>&1)
    
    # 4. Parse the runtime from the output string "Halide (manual):	<number>us".
    local RUNTIME
    RUNTIME=$(echo "$OUTPUT" | grep 'Halide (manual):' | awk '{print $3}' | sed 's/us//' || true)

    # 5. Print the full output of the run script to stderr for context.
    echo "$OUTPUT" >&2

    if [ -z "$RUNTIME" ]; then
        echo "Error: Could not parse runtime from benchmark output." >&2
        exit 1
    fi
    
    # 6. Echo the parsed number to STDOUT. This is the function's "return value".
    echo "$RUNTIME"
}

# --- Main Script Logic ---
echo "=========================================" >&2
echo "  RAW Pipeline Stage Benchmark Script" >&2
echo "=========================================" >&2

# Create a string of all flags set to OFF to ensure a clean reset for each run.
ALL_CMAKE_FLAGS=""
for STAGE_OPTION in "${STAGES[@]}"; do
    ALL_CMAKE_FLAGS+="-DDISABLE_${STAGE_OPTION}=OFF "
done

# Initialize positional array to store results: STAGE_TIMES[0] is baseline.
STAGE_TIMES=()
STAGE_NAMES=("Baseline" "${STAGES[@]}")

# 1. Get the baseline time with all stages enabled.
BASELINE_TIME=$(run_benchmark "")
STAGE_TIMES[0]=$BASELINE_TIME

echo "-----------------------------------------" >&2
echo "Baseline runtime (all stages ON): $BASELINE_TIME us" >&2
echo "-----------------------------------------" >&2

# 2. Iterate through each stage, disable it, and run the benchmark.
for i in "${!STAGES[@]}"; do
    STAGE_OPTION="${STAGES[i]}"
    CMAKE_FLAG="-DDISABLE_${STAGE_OPTION}=ON"
    STAGE_TIME=$(run_benchmark "$CMAKE_FLAG")
    # Store the result in the positional array, starting at index 1
    STAGE_TIMES[$((i+1))]=$STAGE_TIME
    echo "-----------------------------------------" >&2
done

# 3. Print the final summary report. This goes to stdout.
echo
echo "============================ BENCHMARK SUMMARY ============================"
printf "%-35s | %15s | %15s\n" "Configuration" "Runtime (us)" "Stage Cost (us)"
printf "%-35s | %15s | %15s\n" "-----------------------------------" "---------------" "---------------"

BASELINE=${STAGE_TIMES[0]}

# Print Baseline row
printf "%-35s | %15s | %15s\n" "All Stages Enabled (Baseline)" "$BASELINE" "-"

# Print results for each disabled stage
for i in "${!STAGES[@]}"; do
    STAGE_NAME="${STAGES[i]}"
    RUNTIME=${STAGE_TIMES[$((i+1))]}
    # Cost = (Baseline Time) - (Time with Stage Disabled)
    COST=$((BASELINE - RUNTIME))
    printf "%-35s | %15s | %15s\n" "Disabled: $STAGE_NAME" "$RUNTIME" "$COST"
done
echo "========================================================================="
echo "* 'Stage Cost' is the performance improvement gained by disabling the stage."

# Reset all flags to OFF for the next manual build.
cmake -B "$BUILD_DIR" $ALL_CMAKE_FLAGS > /dev/null
