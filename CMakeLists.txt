cmake_minimum_required(VERSION 3.10)  # Specify the minimum version for CMake.

project(U-TRR LANGUAGES CXX)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Add subdirectories
add_subdirectory(src)
# add_subdirectory(libs)
# add_subdirectory(tests)
