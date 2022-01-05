cmake_minimum_required (VERSION 3.18)

project(libdemo VERSION 0.0)

set(module_name "libdemo")

# Target
add_library(
    ${module_name} MODULE 
    "${project_ext_dir}/imgui/imgui.cpp"
    "${project_ext_dir}/imgui/imgui_demo.cpp"
    "${project_ext_dir}/imgui/imgui_draw.cpp"
    "${project_ext_dir}/imgui/imgui_tables.cpp"
    "${project_ext_dir}/imgui/imgui_widgets.cpp"
    "${project_ext_dir}/imgui/backends/imgui_impl_win32.cpp"
    "${project_ext_dir}/directXTK/src/SimpleMath.cpp"
    "${project_ext_dir}/MikkTSpace/mikktspace.c"
    "src/demo.cpp"
    "src/backend-d3d12.cpp" 
    "src/shadercompiler.cpp"
    "src/renderer.cpp" 
    "src/profiling.cpp" 
    "src/mesh-utils.cpp")

target_compile_options(${module_name} PUBLIC /await)

set_property(TARGET ${module_name} PROPERTY CXX_STANDARD 20)

# Include path
include_directories(
    "inc"
    "${project_ext_dir}"
    "${project_ext_dir}/spookyhash/inc"
    "${project_ext_dir}/dxc/inc"
    "${project_ext_dir}/imgui"
    "${project_ext_dir}/tinygltf"
    "${project_ext_dir}/json"
    "${project_ext_dir}/directXTK/inc"
    "${project_ext_dir}/winpixeventruntime.1.0.210209001/Include/WinPixEventRuntime"
    "${project_ext_dir}/directXTex/inc"
    "${project_ext_dir}/d3d12.1.600.10/include"
    "${project_ext_dir}/tracy")

# Macro defines
add_compile_definitions(
    UNICODE
    _UNICODE
    TRACY_ENABLE
    TRACY_IMPORTS
    NOMINMAX                                                                                # Disable Windows.h from defining max() min() macros which collide with other code include the standard library
    SHADER_DIR=L"${project_src_dir}/demo-dll/shaders"
    SRC_INCLUDE_DIR=L"${project_src_dir}/demo-dll/inc"
    CONTENT_DIR="${project_root_dir}/content")

# Generate a unique name
string(TIMESTAMP seed %s)
string(RANDOM LENGTH 6 RANDOM_SEED ${seed} suffix)
string(CONCAT unique_name ${module_name} "_" ${suffix})

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug" AND
    EXISTS ${project_bin_dir}/${module_name}.pdb)
    # Move PDB file to new location so that we can rebuild while file is locked. VS allows that for locked files.
    add_custom_command(
            TARGET libdemo PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E rename
                    ${project_bin_dir}/${module_name}.pdb
                    ${project_bin_dir}/${unique_name}.pdb)

    # Copy contents to old location so that a rebuild isn't forced due to a missing pdb
    add_custom_command(
            TARGET libdemo PRE_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${project_bin_dir}/${unique_name}.pdb
                    ${project_bin_dir}/${module_name}.pdb)
endif()

if(${CMAKE_BUILD_TYPE} STREQUAL "Debug")
    target_link_directories(
        libdemo 
        PRIVATE "${project_bin_dir}"
        PRIVATE "${project_ext_dir}/spookyhash/lib"
        PRIVATE "${project_ext_dir}/dxc/lib/x64"
        PRIVATE "${project_ext_dir}/winpixeventruntime.1.0.210209001/bin/x64"
        PRIVATE "${project_ext_dir}/directXTex/lib/x64/debug")
else()
    target_link_directories(
        libdemo 
        PRIVATE "${project_bin_dir}"
        PRIVATE "${project_ext_dir}/spookyhash/lib"
        PRIVATE "${project_ext_dir}/dxc/lib/x64"
        PRIVATE "${project_ext_dir}/winpixeventruntime.1.0.210209001/bin/x64"
        PRIVATE "${project_ext_dir}/directXTex/lib/x64/release")

endif()

target_link_libraries(
    libdemo 
    PRIVATE dxgi.lib
    PRIVATE dxguid.lib
    PRIVATE d3d12.lib
    PRIVATE Ws2_32.lib
    PRIVATE dxcompiler.lib
    PRIVATE spookyhash.lib
    PRIVATE WinPixEventRuntime.lib
    PRIVATE DirectXTex.lib
    PRIVATE tracy.lib)

# Ensure that tracy.lib is built before trying to link the dll
add_dependencies(libdemo tracy)
