cmake_minimum_required(VERSION 3.15)
project(json_block CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nlohmann/json
include_directories(../../external/json/include)

# Build as shared library
add_library(json_block SHARED json_block.cpp)

# Set output directory
set_target_properties(json_block PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    PREFIX "lib"
    SUFFIX ".so"
)

message(STATUS "BLOCK-JSON will be built as: libjson_block.so")
