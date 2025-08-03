#!/bin/bash

# rm out*.png

set -e
./build/process \
  --input bayer_raw.png \
  --output out.png \
  --color-temp 3700 \
  --tint 0.1 \
  --contrast 55 \
  --sharpen 1.2 \
  --ca-strength 1.0 \
  --exposure 1.0 \
  --saturation 1.0 \
  --saturation-algo hsl \
  --tonemap linear \
  --demosaic ahd \
  --iterations 10 \
  --white-point 1000 \
  --exposure 2.0 \
  --curve-points "0.25:0.22,0.75:0.8" \

# open out.png
# open out-curves.png
