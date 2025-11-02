# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2
LDFLAGS =

# --- Project Structure ---
EXECUTABLE = open_rf_liveview
SRC_DIR = src
IMGUI_DIR = $(SRC_DIR)/vendor/imgui
ASSET_DIR = assets
BUILD_DIR = build

# --- Dear ImGui Setup ---
# Use a specific, stable version tag from the ImGui repository
IMGUI_VERSION = v1.90.5
IMGUI_BASE_URL = https://raw.githubusercontent.com/ocornut/imgui/$(IMGUI_VERSION)
IMGUI_FILES = \
    imconfig.h imgui.cpp imgui_draw.cpp imgui.h imgui_internal.h \
    imgui_tables.cpp imgui_widgets.cpp \
    imstb_rectpack.h imstb_textedit.h imstb_truetype.h
IMGUI_BACKEND_FILES = \
    imgui_impl_opengl3.cpp imgui_impl_opengl3.h imgui_impl_opengl3_loader.h
# Sentinel file to check if ImGui has been downloaded
IMGUI_SENTINEL = $(IMGUI_DIR)/.download_complete

# --- Source and Object Files ---
# List application sources with wildcard
APP_SOURCES = $(wildcard $(SRC_DIR)/*.cpp)

# Explicitly list the ImGui source files we download
IMGUI_CPP_FILES = \
    imgui.cpp \
    imgui_draw.cpp \
    imgui_tables.cpp \
    imgui_widgets.cpp \
    imgui_impl_opengl3.cpp
IMGUI_SOURCES = $(addprefix $(IMGUI_DIR)/, $(IMGUI_CPP_FILES))

# Combine the lists
SOURCES = $(APP_SOURCES) $(IMGUI_SOURCES)

# Object files: create a corresponding .o file in the BUILD_DIR for each source file
# This substitution works for both `src/*.cpp` and `src/vendor/imgui/*.cpp`
OBJECTS = $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))

# --- Dependencies ---
# Libraries to find using pkg-config. On Raspberry Pi OS, OpenCV is opencv4.
PKG_LIBS = opencv4 glesv2 egl libdrm gbm udev

# Use pkg-config to get compiler and linker flags for our libraries
CXXFLAGS += $(shell pkg-config --cflags $(PKG_LIBS))
LIBS = $(shell pkg-config --libs $(PKG_LIBS))

# Include our own source and vendor directories for headers
CXXFLAGS += -I$(SRC_DIR) -I$(IMGUI_DIR) -I$(ASSET_DIR)


# --- Build Rules ---

# The default target, called when you just run 'make'.
# Depends on the executable being built.
all: $(EXECUTABLE)

# Rule to link the final executable.
# Depends on all object files being compiled.
$(EXECUTABLE): $(OBJECTS)
	@echo "==> Linking $@..."
	$(CXX) $(LDFLAGS) -o $@ $^ $(LIBS)

# Pattern rule to compile .cpp source files into .o object files.
# This rule depends on the ImGui sources being present before compiling.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(IMGUI_SENTINEL)
	@mkdir -p $(@D) # Create build directory if it doesn't exist
	@echo "==> Compiling $<..."
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# A "phony" target is one that is not a file.
.PHONY: all clean install-deps fetch-imgui

# --- Setup and Maintenance Rules ---

# Rule to install system-level package dependencies using apt-get.
install-deps:
	@echo "==> Installing system dependencies (requires sudo)..."
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
	    libegl1-mesa-dev

# Rule to download Dear ImGui source files.
# This rule is triggered automatically by the build if the files are missing.
fetch-imgui: $(IMGUI_SENTINEL)

$(IMGUI_SENTINEL):
	@echo "==> Dear ImGui not found. Downloading..."
	@mkdir -p $(IMGUI_DIR)
	@for file in $(IMGUI_FILES); do \
	    echo "    Fetching $$file..."; \
	    wget -qO $(IMGUI_DIR)/$$file $(IMGUI_BASE_URL)/$$file; \
	done
	@for file in $(IMGUI_BACKEND_FILES); do \
	    echo "    Fetching backend/$$file..."; \
	    wget -qO $(IMGUI_DIR)/$$file $(IMGUI_BASE_URL)/backends/$$file; \
	done
	@touch $@ # Create the sentinel file to mark completion

# Rule to clean up build artifacts.
clean:
	@echo "==> Cleaning up..."
	rm -f $(EXECUTABLE)
	rm -rf $(BUILD_DIR)
	rm -rf $(IMGUI_DIR) # Also remove downloaded ImGui files
