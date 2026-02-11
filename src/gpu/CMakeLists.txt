# Copyright (c) 2025 The Freycoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Freycoin GPU Acceleration Library
#
# Supports two backends:
# - OpenCL: dynamic loading at runtime, works on all GPU vendors (NVIDIA, AMD, Intel)
# - Metal: native Apple Silicon GPU compute with zero-copy unified memory

# Collect source files
set(GPU_SOURCES
    opencl_loader.h
    opencl_loader.cpp
    opencl_fermat.h
    opencl_fermat.cpp
    fermat_cl_source.h
    gpu_fermat.h
    gpu_fermat.cpp
)

# Apple Metal backend
if(APPLE)
    # Compile the Metal shader into a .metallib, then embed as a C byte array
    set(METAL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/fermat_metal.metal")
    set(METAL_AIR "${CMAKE_CURRENT_BINARY_DIR}/fermat_metal.air")
    set(METAL_LIB "${CMAKE_CURRENT_BINARY_DIR}/fermat.metallib")
    set(METAL_EMBED_H "${CMAKE_CURRENT_BINARY_DIR}/fermat_metallib.h")

    # Step 1: .metal -> .air (Metal IR)
    add_custom_command(
        OUTPUT "${METAL_AIR}"
        COMMAND xcrun -sdk macosx metal
            -c "${METAL_SOURCE}"
            -o "${METAL_AIR}"
            -std=metal3.0
            -O2
        DEPENDS "${METAL_SOURCE}"
        COMMENT "Compiling Metal shader: fermat_metal.metal -> .air"
        VERBATIM
    )

    # Step 2: .air -> .metallib
    add_custom_command(
        OUTPUT "${METAL_LIB}"
        COMMAND xcrun -sdk macosx metallib
            "${METAL_AIR}"
            -o "${METAL_LIB}"
        DEPENDS "${METAL_AIR}"
        COMMENT "Linking Metal library: .air -> fermat.metallib"
        VERBATIM
    )

    # Step 3: .metallib -> embedded C header
    # Uses xxd to create a byte array, then wraps it with proper variable names.
    add_custom_command(
        OUTPUT "${METAL_EMBED_H}"
        COMMAND ${CMAKE_COMMAND} -E echo
            "/* Auto-generated from fermat.metallib — do not edit */"
            > "${METAL_EMBED_H}"
        COMMAND ${CMAKE_COMMAND} -E echo
            "#include <stddef.h>"
            >> "${METAL_EMBED_H}"
        # Use a small Python/shell script to convert binary to C array
        COMMAND ${CMAKE_COMMAND}
            -DINPUT_FILE="${METAL_LIB}"
            -DOUTPUT_FILE="${METAL_EMBED_H}"
            -DARRAY_NAME=fermat_metallib_data
            -DSIZE_NAME=fermat_metallib_size
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_binary.cmake"
        DEPENDS "${METAL_LIB}"
        COMMENT "Embedding fermat.metallib as C array"
        VERBATIM
    )

    # Add Metal sources
    list(APPEND GPU_SOURCES
        metal_fermat.h
        metal_fermat.mm
        "${METAL_EMBED_H}"
    )

    # Mark the .mm file as Objective-C++
    set_source_files_properties(metal_fermat.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
        LANGUAGE OBJCXX
    )
endif()

# Create GPU library
add_library(freycoin_gpu STATIC EXCLUDE_FROM_ALL ${GPU_SOURCES})

# Platform-specific dynamic linking support
if(WIN32)
    # LoadLibrary is in kernel32 (linked automatically)
elseif(APPLE)
    target_link_libraries(freycoin_gpu PRIVATE
        ${CMAKE_DL_LIBS}
        "-framework Metal"
        "-framework Foundation"
    )
else()
    # dlopen/dlsym require -ldl on Linux
    target_link_libraries(freycoin_gpu PRIVATE ${CMAKE_DL_LIBS})
endif()

target_include_directories(freycoin_gpu
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

target_link_libraries(freycoin_gpu
    PRIVATE
        core_interface
)

# Set compile options
target_compile_options(freycoin_gpu PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)
