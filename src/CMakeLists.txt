# Find the Boost package
find_package(Boost REQUIRED COMPONENTS program_options)

# Include Boost headers
include_directories(${Boost_INCLUDE_DIRS})

# Add your executable
add_executable(main main.cpp)

# Link Boost libraries to your executable
target_link_libraries(main ${Boost_LIBRARIES})
