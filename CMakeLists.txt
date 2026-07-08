cmake_minimum_required(VERSION 3.18)
project(wawa VERSION 1.0.0 LANGUAGES C)

# Save our version before SAIL shadows PROJECT_VERSION
set(WAWA_VERSION "${PROJECT_VERSION}")

# ---- Options for SAIL (must be set before add_subdirectory) ----
set(SAIL_BUILD_APPS       OFF CACHE BOOL "Disable SAIL apps")
set(SAIL_BUILD_BINDINGS   OFF CACHE BOOL "Disable SAIL bindings")
set(SAIL_BUILD_EXAMPLES   OFF CACHE BOOL "Disable SAIL examples")
set(SAIL_COMBINE_CODECS   ON  CACHE BOOL "Combine codecs into a single library for simpler deployment")
set(SAIL_THREAD_SAFE      OFF CACHE BOOL "Disable thread safety (wawa is single-threaded)")
set(SAIL_MANIP_USE_SWSCALE OFF CACHE BOOL "Disable libswscale dependency for sail-manip")
set(BUILD_TESTING            OFF CACHE BOOL "Disable SAIL tests")

# ---- RPATH: keep build tree working, strip for install (AUR compliance) ----
# Installed binary/libs must not embed any build directory paths.
# ldconfig resolves the standard /usr/lib/ paths at runtime.
set(CMAKE_SKIP_INSTALL_RPATH ON)

# ---- Machine-specific optimization for ALL targets including SAIL ----
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")

# ---- LTO for ALL targets (SAIL codecs benefit too) ----
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported LANGUAGES C)
if(ipo_supported)
    cmake_policy(SET CMP0069 NEW)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# ---- Add SAIL as a subproject ----
add_subdirectory(sail EXCLUDE_FROM_ALL)

# ---- Find Wayland dependencies ----
find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_check_modules(WAYLAND_SCANNER   REQUIRED wayland-scanner)

execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=wayland_scanner wayland-scanner
    OUTPUT_VARIABLE _wayland_scanner_exe
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --variable=pkgdatadir wayland-protocols
    OUTPUT_VARIABLE _wayland_protocols_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(WAYLAND_SCANNER_EXE      "${_wayland_scanner_exe}")
set(WAYLAND_PROTOCOLS_DIR    "${_wayland_protocols_dir}")

# ---- Generate Wayland protocol stubs ----
set(PROTOCOL_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
)

# wlr-layer-shell (from local XML)
add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.h
        ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXE} client-header
        "${CMAKE_CURRENT_SOURCE_DIR}/wlr-layer-shell-unstable-v1.xml"
        "${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.h"
    COMMAND ${WAYLAND_SCANNER_EXE} private-code
        "${CMAKE_CURRENT_SOURCE_DIR}/wlr-layer-shell-unstable-v1.xml"
        "${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/wlr-layer-shell-unstable-v1.xml"
)

# xdg-shell (from system-installed wayland-protocols)
add_custom_command(
    OUTPUT
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
    COMMAND ${WAYLAND_SCANNER_EXE} client-header
        "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
        "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.h"
    COMMAND ${WAYLAND_SCANNER_EXE} private-code
        "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
        "${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c"
    DEPENDS "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml"
)

# ---- Build wawa ----
add_executable(wawa
    wawa.c
    ${PROTOCOL_SRCS}
)

target_include_directories(wawa PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
)

target_link_libraries(wawa PRIVATE
    sail             # SAIL image loading library
    sail-manip       # SAIL pixel format conversion (RGBA)
    PkgConfig::WAYLAND_CLIENT
    m
)

target_compile_definitions(wawa PRIVATE
    _GNU_SOURCE
    VERSION="${WAWA_VERSION}"
)

# ---- Respect BUILD_SHARED_LIBS default ----
if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ON)
endif()
