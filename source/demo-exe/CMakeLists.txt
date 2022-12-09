cmake_minimum_required (VERSION 3.18)

add_executable (demo WIN32 "main.cpp")

set_property(TARGET demo PROPERTY CXX_STANDARD 20)

include_directories(
	"${project_src_dir}/demo-dll/inc"
	"${project_ext_dir}/tracy")

add_compile_definitions(
	UNICODE 
	_UNICODE
	TRACY_ENABLE
	TRACY_IMPORTS
	NOMINMAX																				# Disable Windows.h from defining max() min() macros which collide with other code include the standard library
	LIB_DEMO_NAME=L"libdemo" 
	PROJECT_BIN_DIR=L"${project_bin_dir}"
	SPOOKYHASH_BIN_DIR=L"${project_ext_dir}/spookyhash/bin"
	DXC_BIN_DIR=L"${project_ext_dir}/dxc/bin/x64"
	PIX_BIN_DIR=L"${project_ext_dir}/winpixeventruntime.1.0.210818001/bin/x64"
	D3D_SDK_RELATIVE_PATH="../../ext/d3d12.1.602.0/bin/x64/")

target_link_directories(demo PRIVATE "${project_bin_dir}")

target_link_libraries(demo PRIVATE tracy.lib)

# Ensure that tracy.lib is built before trying to link the exe
add_dependencies(demo tracy)
