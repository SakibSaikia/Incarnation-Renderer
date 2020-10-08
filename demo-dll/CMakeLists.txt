cmake_minimum_required (VERSION 3.8)

project(libdemo VERSION 0.0)

set(module_name "libdemo")

# Target
add_library(
    ${module_name} MODULE 
    "${CMAKE_SOURCE_DIR}/ext/imgui/imgui.cpp"
    "${CMAKE_SOURCE_DIR}/ext/imgui/imgui_demo.cpp"
    "${CMAKE_SOURCE_DIR}/ext/imgui/imgui_draw.cpp"
    "${CMAKE_SOURCE_DIR}/ext/imgui/imgui_widgets.cpp"
    "${CMAKE_SOURCE_DIR}/ext/imgui/imgui_impl_win32.cpp"
    "${CMAKE_SOURCE_DIR}/ext/microprofile/microprofile.cpp"
    "src/demo.cpp"
    "src/d3d12layer.cpp" 
    "src/shadercompiler.cpp"
    "src/renderer.cpp" 
    "src/profiling.cpp")

set_property(TARGET libdemo PROPERTY CXX_STANDARD 20)

# Include path
include_directories(
    "inc"
    "${CMAKE_SOURCE_DIR}/ext"
    "${CMAKE_SOURCE_DIR}/ext/spookyhash/inc"
    "${CMAKE_SOURCE_DIR}/ext/dxc/inc"
    "${CMAKE_SOURCE_DIR}/ext/imgui"
    "${CMAKE_SOURCE_DIR}/ext/microprofile"
    "${CMAKE_SOURCE_DIR}/ext/pix/Include/WinPixEventRuntime")

# Macro defines
add_compile_definitions(
    UNICODE
    _UNICODE
    MICROPROFILE_GPU_TIMERS_D3D12
    SHADER_DIR=L"${CMAKE_SOURCE_DIR}/demo-dll/shaders")

# Generate a unique name
string(TIMESTAMP seed %s)
string(RANDOM LENGTH 6 RANDOM_SEED ${seed} suffix)
string(CONCAT unique_name ${module_name} "_" ${suffix})

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug" AND
    EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${module_name}.pdb)
    # Move PDB file to new location so that we can rebuild while file is locked. VS allows that for locked files.
    add_custom_command(
            TARGET libdemo PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E rename
                    ${CMAKE_CURRENT_BINARY_DIR}/${module_name}.pdb
                    ${CMAKE_CURRENT_BINARY_DIR}/${unique_name}.pdb)

    # Copy contents to old location so that a rebuild isn't forced due to a missing pdb
    add_custom_command(
            TARGET libdemo PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${CMAKE_CURRENT_BINARY_DIR}/${unique_name}.pdb
                    ${CMAKE_CURRENT_BINARY_DIR}/${module_name}.pdb)
endif()

target_link_directories(
    libdemo 
    PRIVATE "${CMAKE_SOURCE_DIR}/ext/spookyhash/lib"
    PRIVATE "${CMAKE_SOURCE_DIR}/ext/dxc/lib/x64"
    PRIVATE "${CMAKE_SOURCE_DIR}/ext/pix/bin/x64")

target_link_libraries(
    libdemo 
    PRIVATE dxgi.lib
    PRIVATE dxguid.lib
    PRIVATE d3d12.lib
    PRIVATE Ws2_32.lib
    PRIVATE dxcompiler.lib
    PRIVATE spookyhash.lib
    PRIVATE WinPixEventRuntime.lib)
