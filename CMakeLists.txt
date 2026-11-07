cmake_minimum_required(VERSION 3.20)
project(fh3_launch_control VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# The FH3 Mod Platform checkout (only include/fh3mod.h is needed to build a mod).
set(FH3_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/platform" CACHE PATH "FH3 Mod Platform checkout")
if(NOT EXISTS "${FH3_PLATFORM_DIR}/include/fh3mod.h")
    message(FATAL_ERROR "fh3mod.h not found in ${FH3_PLATFORM_DIR}/include; add the platform as ./platform or pass -DFH3_PLATFORM_DIR=...")
endif()
file(STRINGS "${FH3_PLATFORM_DIR}/CMakeLists.txt" _pv REGEX "^project\\(fh3_mod_platform VERSION")
string(REGEX REPLACE ".*VERSION ([0-9.]+).*" "\\1" FH3_PLATFORM_VERSION "${_pv}")

add_library(launch_control SHARED src/launch_control.cpp)
target_include_directories(launch_control PRIVATE "${FH3_PLATFORM_DIR}/include")
target_compile_definitions(launch_control PRIVATE
    LAUNCHCONTROL_VERSION="${PROJECT_VERSION}"
    FH3MOD_PLATFORM_VERSION="${FH3_PLATFORM_VERSION}"
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(NOT MSVC)
    # -fms-extensions: __try/__except around reads of game memory.
    target_compile_options(launch_control PRIVATE -Wall -Wextra -fms-extensions "$<$<CONFIG:Release>:-O2>")
    target_link_options(launch_control PRIVATE -static "$<$<CONFIG:Release>:-s>")
endif()
set_target_properties(launch_control PROPERTIES OUTPUT_NAME LaunchControl PREFIX "")
