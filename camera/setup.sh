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
    libdrm-dev \
    libgbm-dev \
    libudev-dev \
    libopencv-dev \
    libgles2-mesa-dev \
    libegl1-mesa-dev \
    libjpeg-turbo8-dev
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
ROBOTO_URL="https://github.com/googlefonts/roboto-2/raw/refs/heads/main/src/hinted/Roboto-Regular.ttf"
ROBOTO_DEST="assets/Roboto-Regular.ttf"
# Use -s to check if the file exists AND is not empty
if [ ! -s "$ROBOTO_DEST" ]; then
    echo "      -> Downloading Roboto Regular font..."
    wget -O "$ROBOTO_DEST" "$ROBOTO_URL"
else
    echo "      -> Roboto font already exists."
fi

# Font Awesome Icons (OTF)
# We download the free solid font and save it with the name the C++ code expects.
FA_URL="https://github.com/FortAwesome/Font-Awesome/raw/refs/heads/fa-release-6.7.2/otfs/Font%20Awesome%206%20Free-Solid-900.otf"
FA_DEST="assets/FontAwesome6-Solid-900.otf"
# Use -s to check if the file exists AND is not empty
if [ ! -s "$FA_DEST" ]; then
    echo "      -> Downloading Font Awesome 6 Solid icon font..."
    wget -O "$FA_DEST" "$FA_URL"
else
    echo "      -> Font Awesome font already exists."
fi

# Font Awesome Icon Header for C++
# This is a source header, so it belongs in the 'src' directory to be found by the compiler.
ICON_HEADER_URL="https://raw.githubusercontent.com/juliettef/IconFontCppHeaders/main/IconsFontAwesome6.h"
ICON_HEADER_DEST="src/IconsFontAwesome6.h"
# Use -s to check if the file exists AND is not empty
if [ ! -s "$ICON_HEADER_DEST" ]; then
    echo "      -> Downloading Font Awesome C++ header..."
    wget -O "$ICON_HEADER_DEST" "$ICON_HEADER_URL"
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
