cmake_minimum_required(VERSION 3.10)
project(spine-c C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

# Include the common flags (including -fno-rtti)
include(${CMAKE_CURRENT_LIST_DIR}/../flags.cmake)

# Include spine-cpp
add_subdirectory(../spine-cpp ${CMAKE_CURRENT_BINARY_DIR}/spine-cpp)

# Collect all source files
file(GLOB SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*.cpp"
)

# Create the library
add_library(spine-c STATIC ${SOURCES})

# Include directories
target_include_directories(spine-c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link with spine-cpp
target_link_libraries(spine-c PUBLIC spine-cpp)

# Add spine-cpp include directories
target_include_directories(spine-c PUBLIC
    ../spine-cpp/spine-cpp/include
)


# Create test executable only if this is the top-level project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_executable(headless-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/headless-test.c)
    target_link_libraries(headless-test spine-c)
endif()
