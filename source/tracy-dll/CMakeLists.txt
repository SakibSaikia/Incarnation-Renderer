cmake_minimum_required (VERSION 3.18)

project(tracy VERSION 0.0)

set(module_name "tracy")

# Target
add_library(
    ${module_name} MODULE 
    "${CMAKE_SOURCE_DIR}/ext/tracy/TracyClient.cpp")

target_compile_options(
    ${module_name} PUBLIC)

set_property(TARGET ${module_name} PROPERTY CXX_STANDARD 20)

# Include path
include_directories("${CMAKE_SOURCE_DIR}/ext/tracy")

# Macro defines
add_compile_definitions(
    UNICODE
    _UNICODE
    TRACY_ENABLE
    TRACY_EXPORTS
    NOMINMAX)
