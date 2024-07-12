cmake_minimum_required(VERSION 3.10)

project(U-TRR LANGUAGES CXX)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)

# Include tools headers from the overall project root directory
include_directories(${CMAKE_SOURCE_DIR}/tools)


# Add subdirectories
add_subdirectory(RowScout)
add_subdirectory(libs/DRAM-Bender)
# add_subdirectory(tests)
