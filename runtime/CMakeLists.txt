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
    src/wr_settings.c
    src/platform/wr_keys.c
    src/platform/wr_input_script.c
    src/platform/wr_pad.c
    src/win32/kernel32.c
    src/win32/user32.c
    src/win32/dinput8.c
    src/win32/misc.c
    src/libs/miles_bink_silent.c
    src/libs/miles.c
    src/vk/wr_vk.c
    src/audio/wr_audio.c
    src/vk/wr_vk_blit.c
    src/vk/wr_overlay.c
    src/post/shader_pass.c
    src/overlay/status_overlay.c
)

# Status overlays (overlay/status_overlay.c, from option-c): the bundled font
# DejaVu Sans Mono Bold goes into the binary as bytes (made at configure
# time), rasterised with stb_truetype. Only this font is ever read.
set(_font_src ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay/third_party/dejavu/DejaVuSansMono-Bold.ttf)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_font_src})
file(READ ${_font_src} _font_hex HEX)
file(SIZE ${_font_src} _font_len)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _font_hex "${_font_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/gen/dejavu_mono_bold.h
     "/* generated from src/overlay/third_party/dejavu/DejaVuSansMono-Bold.ttf */\n"
     "static const unsigned char s_font_ttf[] = { ${_font_hex} };\n"
     "static const unsigned int s_font_ttf_len = ${_font_len}u;\n")

# Slang shader packs (media/shaders -> data/shaders), from option-c src/shaderpack
add_subdirectory(src/shaderpack ${CMAKE_CURRENT_BINARY_DIR}/shaderpack)

target_include_directories(winrecomp_rt PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen ${CMAKE_CURRENT_SOURCE_DIR}/src/overlay)
target_include_directories(winrecomp_rt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../templates/runtime)
target_link_libraries(winrecomp_rt PUBLIC SDL3::SDL3 xbox_shaderpack m)
set_target_properties(winrecomp_rt PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
    target_compile_options(winrecomp_rt PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fwrapv -fno-strict-aliasing)
    target_compile_definitions(winrecomp_rt PRIVATE _GNU_SOURCE)
endif()
