# Find the Boost package
find_package(Boost REQUIRED COMPONENTS program_options filesystem system)

# Include Boost headers
include_directories(${Boost_INCLUDE_DIRS})

# Include DRAM-Bender headers
include_directories(${CMAKE_SOURCE_DIR}/libs/DRAM-Bender/sources/api)

# Add source files
file(GLOB_RECURSE DRAM_BENDER_SOURCES
        ${CMAKE_SOURCE_DIR}/libs/DRAM-Bender/sources/api/*.c
        ${CMAKE_SOURCE_DIR}/libs/DRAM-Bender/sources/api/*.cpp
)

# Add your executable
add_executable(RowScout RowScout.cpp)

# Link Boost libraries to your executable
target_link_libraries(RowScout ${Boost_LIBRARIES})
