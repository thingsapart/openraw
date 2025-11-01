#!/bin/bash

# Exit immediately if a command exits with a non-zero status.
set -e

echo "--- OpenRF/Cam Frontend Setup Script ---"
echo "This script will install dependencies, download assets, and build the project."
echo

# --- 1. Install System Dependencies ---
echo "[1/4] Installing system dependencies (requires sudo)..."
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    pkg-config \
    wget \
    libsdl2-dev \
    libopencv-dev \
    libgles2-mesa-dev \
    libegl1-mesa-dev
echo "Dependencies installed successfully."
echo

# --- 2. Create Project Directories ---
echo "[2/4] Creating required directories..."
mkdir -p assets
mkdir -p src # The 'make' command will create src/vendor/imgui later
echo "Directories created."
echo

# --- 3. Download Font and Icon Assets ---
echo "[3/4] Downloading font and icon assets..."

# Roboto Font
ROBOTO_URL="https://raw.githubusercontent.com/google/fonts/main/apache/roboto/Roboto-Regular.ttf"
ROBOTO_DEST="assets/Roboto-Regular.ttf"
if [ ! -f "$ROBOTO_DEST" ]; then
    echo "      -> Downloading Roboto Regular font..."
    wget -qO "$ROBOTO_DEST" "$ROBOTO_URL"
else
    echo "      -> Roboto font already exists."
fi

# Font Awesome Icons (OTF)
# We download the free solid font and save it with the name the C++ code expects.
FA_URL="https://cdn.jsdelivr.net/npm/@fortawesome/fontawesome-free@6.5.1/otfs/FontAwesome6-Free-Solid-900.otf"
FA_DEST="assets/FontAwesome6-Solid-900.otf"
if [ ! -f "$FA_DEST" ]; then
    echo "      -> Downloading Font Awesome 6 Solid icon font..."
    wget -qO "$FA_DEST" "$FA_URL"
else
    echo "      -> Font Awesome font already exists."
fi

# Font Awesome Icon Header for C++
# This is a source header, so it belongs in the 'src' directory to be found by the compiler.
ICON_HEADER_URL="https://raw.githubusercontent.com/juliettef/IconFontCppHeaders/main/IconsFontAwesome6.h"
ICON_HEADER_DEST="src/IconsFontAwesome6.h"
if [ ! -f "$ICON_HEADER_DEST" ]; then
    echo "      -> Downloading Font Awesome C++ header..."
    wget -qO "$ICON_HEADER_DEST" "$ICON_HEADER_URL"
else
    echo "      -> Font Awesome header already exists."
fi

echo "Assets and headers downloaded successfully."
echo

# --- 4. Build the Project ---
echo "[4/4] Building the project using 'make'..."
echo "This will also download Dear ImGui sources if they are missing."
make fetch-imgui
make

echo
echo "--- Setup Complete! ---"
echo
echo "To run the application, execute:"
echo "  ./openrf_cam"
echo
echo "To clean up all built files and downloaded libraries, run:"
echo "  make clean"
echo
