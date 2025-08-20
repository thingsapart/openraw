#!/bin/bash
# rm -rf build
cmake -S . -B build -DCMAKE_HALIDE_TARGET=host-profile -D CMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang -D CMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
# mkdir build && cd build
# cmake ..
cd build
cmake --build .
