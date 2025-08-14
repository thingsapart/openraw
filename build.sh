#!/bin/bash
# rm -rf build
cmake -S . -B build -DCMAKE_HALIDE_TARGET=host-profile
# mkdir build && cd build
# cmake ..
cd build
cmake --build .
