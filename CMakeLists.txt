cmake_minimum_required(VERSION 3.10)

project(U-TRR LANGUAGES CXX)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/tools)
include_directories(${CMAKE_SOURCE_DIR}/libs/DRAM-Bender/sources/api)

# Find the Boost package
find_package(Boost REQUIRED COMPONENTS program_options filesystem system)

# Include Boost headers
include_directories(${Boost_INCLUDE_DIRS})

# Add source files
file(GLOB_RECURSE DRAM_BENDER_SOURCES
        ${CMAKE_SOURCE_DIR}/libs/DRAM-Bender/sources/api/*.c
        ${CMAKE_SOURCE_DIR}/libs/DRAM-Bender/sources/api/*.cpp
)

# Add your executable
add_executable(RowScoutExecutable src/RowScout/RowScout.cpp ${DRAM_BENDER_SOURCES})
# Link Boost and other libraries to your executable
target_link_libraries(RowScoutExecutable ${Boost_LIBRARIES} pthread)

# Add your executable
add_executable(TRRAnalyzerExecutable src/TRRAnalyzer/TRRAnalyzer.cpp ${DRAM_BENDER_SOURCES}
        src/RowGroupSelector.cpp)
# Link Boost and other libraries to your executable
target_link_libraries(TRRAnalyzerExecutable ${Boost_LIBRARIES} pthread)
