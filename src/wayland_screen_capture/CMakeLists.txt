find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(
    WAYLAND_SCREEN_CAPTURE_DEPS
    REQUIRED
    IMPORTED_TARGET
    gio-2.0
    libportal
    libpipewire-0.3
    libspa-0.2
)

add_library(
    wayland_screen_capture
    wayland_capture_session.cpp
)

add_library(
    wayland_screen_capture::wayland_screen_capture
    ALIAS
    wayland_screen_capture
)

target_include_directories(
    wayland_screen_capture
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(
    wayland_screen_capture
    PUBLIC
    cxx_std_17
)

target_link_libraries(
    wayland_screen_capture
    PRIVATE
    PkgConfig::WAYLAND_SCREEN_CAPTURE_DEPS
    Threads::Threads
)

set_target_properties(
    wayland_screen_capture
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)