cmake_minimum_required(VERSION 3.16)

project(
    wayland_screen_capture
    VERSION 0.1.0
    LANGUAGES CXX
)

set(WAYLAND_SCREEN_CAPTURE_IS_TOP_LEVEL OFF)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(WAYLAND_SCREEN_CAPTURE_IS_TOP_LEVEL ON)
endif()

option(
    WAYLAND_SCREEN_CAPTURE_BUILD_EXAMPLES
    "Build wayland-screen-capture examples"
    ${WAYLAND_SCREEN_CAPTURE_IS_TOP_LEVEL}
)

add_subdirectory(
    src/wayland_screen_capture
)

if(WAYLAND_SCREEN_CAPTURE_BUILD_EXAMPLES)
    add_subdirectory(
        examples/capture_probe
    )
endif()