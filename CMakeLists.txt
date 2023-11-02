cmake_minimum_required (VERSION 3.26)

project(demo VERSION 0.1)

# Project path definitions
set(project_root_dir "${CMAKE_SOURCE_DIR}")
set(project_bin_dir "${CMAKE_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(project_ext_dir "${CMAKE_SOURCE_DIR}/ext")
set(project_src_dir "${CMAKE_SOURCE_DIR}/source")
set(project_content_dir "${CMAKE_SOURCE_DIR}/content")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${project_bin_dir}) # EXE
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${project_bin_dir}) # DLL
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${project_bin_dir}) # LIB

add_subdirectory(source/tracy-dll)
add_subdirectory(source/demo-dll)
add_subdirectory(source/demo-exe)