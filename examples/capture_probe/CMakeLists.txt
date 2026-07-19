add_executable(
    wayland_capture_probe
    main.cpp
)

target_link_libraries(
    wayland_capture_probe
    PRIVATE
    wayland_screen_capture::wayland_screen_capture
)