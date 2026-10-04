cmake_minimum_required(VERSION 3.21)

# Default the VS generator to clang-cl (LLVM toolset). An explicit -T on the
# command line wins because its value is already set when this file is read.
if(CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT CMAKE_GENERATOR_TOOLSET)
    set(CMAKE_GENERATOR_TOOLSET "ClangCL")
endif()

project(rv LANGUAGES CXX)

# Both targets are 64-bit only (Ride-Win64 + x64 launcher).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "x64 build required: configure with -A x64 (VS generator).")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "MSVC toolchain required: use the VS generator or an x64 developer prompt.")
endif()

# /std:c++latest /W3 /utf-8 (/O2, /EHsc, /INCREMENTAL:NO come with Release).
add_compile_options(/std:c++latest /W3 /utf-8)

# Injected DLL. Output name is deliberately plain: output.dll.
add_library(rv SHARED
    src/rv/main.cpp
    src/rv/resolve.hpp
    src/rv/signatures.hpp
    src/rv/unreal.hpp)
target_compile_options(rv PRIVATE /wd4369 /wd4309)
set_target_properties(rv PROPERTIES
    OUTPUT_NAME "output"
    MSVC_RUNTIME_LIBRARY "MultiThreaded")

# dllbytes.h: embed the freshly built DLL as DLL_BYTES[] / DLL_SIZE.
set(generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
    OUTPUT "${generated_dir}/dllbytes.h"
    COMMAND "${CMAKE_COMMAND}"
            "-DSRC=$<TARGET_FILE:rv>"
            "-DOUT=${generated_dir}/dllbytes.h"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_dll.cmake"
    DEPENDS rv "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_dll.cmake"
    COMMENT "Embedding output.dll into dllbytes.h"
    VERBATIM)

# Injector exe. No OUTPUT_NAME: it lands as launcher.exe (target name).
add_executable(launcher
    src/launcher/main.cpp
    "${generated_dir}/dllbytes.h")
target_compile_definitions(launcher PRIVATE UNICODE _UNICODE)
target_include_directories(launcher PRIVATE "${generated_dir}")
target_link_libraries(launcher PRIVATE user32 advapi32)
set_target_properties(launcher PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded")

# Optional UPX pack, silently skipped when upx is absent.
# The cache variable must NOT be named UPX: upx.exe reads an env var called UPX
# as its own option string and dies on a path value.
option(RV_UPX_PACK "UPX-pack launcher.exe after build" ON)
find_program(UPX_EXECUTABLE NAMES upx HINTS "${CMAKE_CURRENT_SOURCE_DIR}")
if(RV_UPX_PACK AND UPX_EXECUTABLE)
    add_custom_command(TARGET launcher POST_BUILD
        COMMAND "${UPX_EXECUTABLE}" -9 --quiet "$<TARGET_FILE:launcher>"
        COMMENT "UPX-packing launcher.exe")
endif()
