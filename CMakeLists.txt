# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(Hawk VERSION 0.0.1 LANGUAGES C)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Hawk currently builds only on Windows with MSVC (cl) or clang-cl.")
endif()

option(HK_ASAN "Build with AddressSanitizer" OFF)
option(HK_FUZZ "Build fuzz targets only (implies HK_ASAN)" OFF)
if(HK_FUZZ)
    set(HK_ASAN ON CACHE BOOL "" FORCE)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT: no vcruntime DLL dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

# Options shared by every Hawk target.
add_library(hk_options INTERFACE)
target_compile_options(hk_options INTERFACE /W4 /WX /utf-8)
target_compile_definitions(hk_options INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00 PSAPI_VERSION=2)
target_link_options(hk_options INTERFACE
    /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)

if(HK_ASAN)
    add_compile_options(/fsanitize=address)
    set(CMAKE_MSVC_RUNTIME_CHECKS "")
    string(REGEX REPLACE "/RTC[1csu]*" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    add_link_options(/INCREMENTAL:NO)
else()
    # Control Flow Guard (kept off under ASan, which instruments calls itself).
    target_compile_options(hk_options INTERFACE /guard:cf)
    target_link_options(hk_options INTERFACE /guard:cf)
endif()

if(HK_FUZZ)
    add_compile_options(/fsanitize=fuzzer)
endif()

# Copy the ASan runtime next to an executable so it starts outside a VS developer shell.
function(hk_copy_runtime target)
    if(HK_ASAN)
        get_filename_component(_cl_dir "${CMAKE_C_COMPILER}" DIRECTORY)
        foreach(_dll clang_rt.asan_dynamic-x86_64.dll clang_rt.asan_dbg_dynamic-x86_64.dll)
            if(EXISTS "${_cl_dir}/${_dll}")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${_cl_dir}/${_dll}" "$<TARGET_FILE_DIR:${target}>")
            endif()
        endforeach()
    endif()
endfunction()

add_subdirectory(src)

if(HK_FUZZ)
    add_subdirectory(fuzz)
else()
    enable_testing()
    add_subdirectory(tests)
endif()
