cmake_minimum_required (VERSION 3.18)

project(demo VERSION 0.0)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE})

add_subdirectory(tracy-dll)
add_subdirectory(demo-dll)
add_subdirectory(demo-exe)