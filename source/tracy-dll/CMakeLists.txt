cmake_minimum_required (VERSION 3.26)

project(tracy)

set(module_name "tracy")

# Target
add_library(
    ${module_name} MODULE 
    "${project_ext_dir}/tracy/public/TracyClient.cpp")

target_compile_options(
    ${module_name} PUBLIC)

set_property(TARGET ${module_name} PROPERTY CXX_STANDARD 20)

# Include path
include_directories("${project_ext_dir}/tracy/public/tracy")

# Macro defines
add_compile_definitions(
    UNICODE
    _UNICODE
    TRACY_ENABLE
    TRACY_EXPORTS
    NOMINMAX)
