#ifndef WAYLAND_CAPTURE_SESSION_H
#define WAYLAND_CAPTURE_SESSION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

class WaylandCaptureSession {
public:
    enum class SourceType { Monitor, Window };

    enum class CursorMode { Hidden, Embedded };

    enum class State { Idle, Starting, Streaming, Paused, Stopping, Error };

    struct Config {
        SourceType sourceType = SourceType::Monitor;
        CursorMode cursorMode = CursorMode::Embedded;
    };

    struct Frame {
        // 固定为 BGRA/BGRx，每个像素 4 字节。
        // 只在 onFrame() 回调执行期间有效。
        const std::uint8_t* data = nullptr;

        std::uint32_t width = 0;
        std::uint32_t height = 0;

        std::int32_t rowStrideInBytes = 0;
        bool isContiguous = false;

        std::int64_t ptsNs = 0;
        std::uint64_t sequence = 0;
    };

    using FrameCallback = std::function<void(const Frame& frame)>;

    using StateCallback = std::function<void(State state)>;

    using ErrorCallback = std::function<void(std::string_view message)>;

    struct Callbacks {
        FrameCallback onFrame;
        StateCallback onStateChanged;
        ErrorCallback onError;
    };

    WaylandCaptureSession();
    ~WaylandCaptureSession();
    WaylandCaptureSession(const WaylandCaptureSession&) = delete;
    WaylandCaptureSession& operator=(const WaylandCaptureSession&) = delete;

    bool start(const Config& config, Callbacks callbacks);
    void stop();
    void pause();
    void resume();

    bool isPaused() const;
    State state() const;
    bool isRunning() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

#endif