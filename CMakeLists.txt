cmake_minimum_required(VERSION 3.10)

project(U-TRR LANGUAGES CXX)

# Specify C++ standard
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/include)
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

# Add source files for the general src directory
file(GLOB SRC_SOURCES
        ${PROJECT_SOURCE_DIR}/src/*.cpp
)

# Add RowScout specific sources
file(GLOB ROW_SCOUT_SOURCES
        ${PROJECT_SOURCE_DIR}/src/RowScout/*.cpp
)

# Add TRRAnalyzer specific sources
file(GLOB TRR_ANALYZER_SOURCES
        ${PROJECT_SOURCE_DIR}/src/TRRAnalyzer/*.cpp
)

# Add RowScoutExecutable
add_executable(RowScoutExecutable ${SRC_SOURCES} ${ROW_SCOUT_SOURCES} ${DRAM_BENDER_SOURCES})
target_link_libraries(RowScoutExecutable ${Boost_LIBRARIES} pthread)

# Add TRRAnalyzerExecutable
add_executable(TRRAnalyzerExecutable ${SRC_SOURCES} ${TRR_ANALYZER_SOURCES} ${DRAM_BENDER_SOURCES})
target_link_libraries(TRRAnalyzerExecutable ${Boost_LIBRARIES} pthread)
