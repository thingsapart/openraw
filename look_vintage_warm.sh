#!/bin/bash

# --- Film Look: Vintage Warm ---
# Creates a soft, warm, slightly desaturated look with lifted blacks,
# reminiscent of aged color film from the 1970s.

# Check for input file
if [ -z "$1" ]; then
    echo "Usage: ./look_vintage_warm.sh <input_raw.png>"
    exit 1
fi

# Define output filename
INPUT_FILE=$1
BASENAME=$(basename -- "$INPUT_FILE")
FILENAME="${BASENAME%.*}"
OUTPUT_FILE="${FILENAME}_vintage_warm.png"

echo "Processing $INPUT_FILE -> $OUTPUT_FILE"

# Run the process command with a curated set of parameters
./build/process \
    --input "$INPUT_FILE" \
    --output "$OUTPUT_FILE" \
    --demosaic ahd \
    --color-temp 3400 \
    --tint -0.05 \
    --saturation 0.85 \
    --contrast 40 \
    --gamma 2.4 \
    --black-point 40 \
    --white-point 4050

echo "Done."
