#!/bin/bash

# --- Film Look: Cross Process ---
# Creates a high-contrast, high-saturation look with a strong blue/magenta color cast,
# reminiscent of cross-processing slide film in the wrong chemicals.

# Check for input file
if [ -z "$1" ]; then
    echo "Usage: ./look_cross_process.sh <input_raw.png>"
    exit 1
fi

# Define output filename
INPUT_FILE=$1
BASENAME=$(basename -- "$INPUT_FILE")
FILENAME="${BASENAME%.*}"
OUTPUT_FILE="${FILENAME}_cross_process.png"

echo "Processing $INPUT_FILE -> $OUTPUT_FILE"

# Run the process command with a curated set of parameters
./build/process \
    --input "$INPUT_FILE" \
    --output "$OUTPUT_FILE" \
    --demosaic ahd \
    --color-temp 8500 \
    --tint 0.15 \
    --saturation 1.3 \
    --contrast 80 \
    --black-point 30 \
    --sharpen 1.2 \
    --exposure 3.5

echo "Done."
