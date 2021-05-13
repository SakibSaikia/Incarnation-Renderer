cmake_minimum_required (VERSION 3.18)

add_executable (demo WIN32 "main.cpp")

set_property(TARGET demo PROPERTY CXX_STANDARD 20)

include_directories("../demo-dll/inc")

add_compile_definitions(
	UNICODE 
	_UNICODE
	LIB_DEMO_NAME=L"libdemo" 
	LIB_DEMO_DIR=L"${CMAKE_BINARY_DIR}/demo-dll" 
	SPOOKYHASH_BIN_DIR=L"${CMAKE_SOURCE_DIR}/ext/spookyhash/bin"
	DXC_BIN_DIR=L"${CMAKE_SOURCE_DIR}/ext/dxc/bin/x64"
	PIX_BIN_DIR=L"${CMAKE_SOURCE_DIR}/ext/winpixeventruntime.1.0.210209001/bin/x64"
	D3D_SDK_RELATIVE_PATH="../../../ext/d3d12.1.4.9/bin/x64/")

target_link_directories(demo PUBLIC "${CMAKE_BINARY_DIR}")
