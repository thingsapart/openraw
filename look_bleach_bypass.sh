#!/bin/bash

# --- Film Look: Bleach Bypass ---
# Creates a gritty, desaturated, high-contrast look with crushed blacks and
# sharp details, similar to the bleach bypass process used in action films.

# Check for input file
if [ -z "$1" ]; then
    echo "Usage: ./look_bleach_bypass.sh <input_raw.png>"
    exit 1
fi

# Define output filename
INPUT_FILE=$1
BASENAME=$(basename -- "$INPUT_FILE")
FILENAME="${BASENAME%.*}"
OUTPUT_FILE="${FILENAME}_bleach_bypass.png"

echo "Processing $INPUT_FILE -> $OUTPUT_FILE"

# Run the process command with a curated set of parameters
./build/process \
    --input "$INPUT_FILE" \
    --output "$OUTPUT_FILE" \
    --demosaic ahd \
    --color-temp 5500 \
    --saturation 0.3 \
    --contrast 95 \
    --black-point 20 \
    --white-point 4000 \
    --sharpen 1.6 \
    --tonemap reinhard

echo "Done."
