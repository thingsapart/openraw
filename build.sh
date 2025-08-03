#!/bin/bash
if command -v "brew" >/dev/null 2>&1; then
  ${HALIDE_PATH:`brew --prefix halide`}
fi
rm -rf build
mkdir build; cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
