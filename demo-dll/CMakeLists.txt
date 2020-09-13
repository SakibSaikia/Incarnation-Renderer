cmake_minimum_required (VERSION 3.8)

project(libdemo VERSION 0.0)

set(module_name "libdemo")

# Target
add_library(
    ${module_name} MODULE 
    "src/demo.cpp")

# Include path
include_directories(
    "inc"
    "../ext/enkiTS/inc")

# Macro defines
add_compile_definitions(
    UNICODE
    _UNICODE)

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
