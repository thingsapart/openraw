# This script is called by FetchContent's PATCH_COMMAND.
# The main CMakeLists.txt passes the correct source directory in the
# RAWSPEED_SOURCE_DIR variable.
set(RAWSPEED_CMAKE_FILE "${RAWSPEED_SOURCE_DIR}/src/librawspeed/CMakeLists.txt")

message(STATUS "Patching RawSpeed CMakeLists.txt for shared library build: ${RAWSPEED_CMAKE_FILE}")

# Read the original file
file(READ ${RAWSPEED_CMAKE_FILE} RAWSPEED_CMAKE_CONTENT)

# Define the strings for replacement.
set(SEARCH_STRING "rawspeed_add_library(rawspeed STATIC")
set(REPLACE_STRING "rawspeed_add_library(rawspeed SHARED")

# Replace the specific 'STATIC' keyword in their custom function with 'SHARED'.
string(REPLACE "${SEARCH_STRING}" "${REPLACE_STRING}" RAWSPEED_CMAKE_CONTENT_PATCHED ${RAWSPEED_CMAKE_CONTENT})

# This is the crucial verification step. If the replacement did not change the
# file content, it means the patch target was not found, and we should fail loudly.
if("${RAWSPEED_CMAKE_CONTENT}" STREQUAL "${RAWSPEED_CMAKE_CONTENT_PATCHED}")
    message(FATAL_ERROR "Failed to patch RawSpeed's CMakeLists.txt. The string '${SEARCH_STRING}' was not found. The build system of the dependency may have changed.")
endif()

# Write the patched content back to the file
file(WRITE ${RAWSPEED_CMAKE_FILE} "${RAWSPEED_CMAKE_CONTENT_PATCHED}")

message(STATUS "RawSpeed patching complete.")

