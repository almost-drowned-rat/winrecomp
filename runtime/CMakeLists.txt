# winrecomp runtime: guest memory, PE loader, Win32 layer (SDL3).
cmake_minimum_required(VERSION 3.20)
project(winrecomp_runtime C)

find_package(SDL3 REQUIRED CONFIG)

add_library(winrecomp_rt STATIC
    src/wr_core.c
    src/wr_crash.c
    src/wr_mem.c
    src/wr_heap.c
    src/wr_pe.c
    src/wr_paths.c
    src/platform/wr_keys.c
    src/win32/kernel32.c
    src/win32/user32.c
    src/win32/dinput8.c
    src/win32/misc.c
    src/libs/miles_bink_silent.c
    src/vk/wr_vk.c
)
target_include_directories(winrecomp_rt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../templates/runtime)
target_link_libraries(winrecomp_rt PUBLIC SDL3::SDL3 m)
set_target_properties(winrecomp_rt PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(winrecomp_rt PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fwrapv -fno-strict-aliasing)
    target_compile_definitions(winrecomp_rt PRIVATE _GNU_SOURCE)
endif()
