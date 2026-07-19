#include "wayland_capture_session.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

const char* stateName(WaylandCaptureSession::State state) {
    using State = WaylandCaptureSession::State;

    switch (state) {
    case State::Idle:
        return "Idle";

    case State::Starting:
        return "Starting";

    case State::Streaming:
        return "Streaming";

    case State::Paused:
        return "Paused";

    case State::Stopping:
        return "Stopping";

    case State::Error:
        return "Error";
    }

    return "Unknown";
}

bool saveFrameAsPpm(const WaylandCaptureSession::Frame& frame, const char* path) {
    if (frame.data == nullptr || frame.width == 0 || frame.height == 0 ||
        frame.rowStrideInBytes <= 0) {
        std::cerr << "invalid frame\n";
        return false;
    }

    const std::size_t minimumStride = static_cast<std::size_t>(frame.width) * 4;

    if (static_cast<std::size_t>(frame.rowStrideInBytes) < minimumStride) {
        std::cerr << "invalid frame stride\n";
        return false;
    }

    std::ofstream output(path, std::ios::binary);

    if (!output) {
        std::cerr << "failed to open output file: " << path << '\n';
        return false;
    }

    output << "P6\n" << frame.width << ' ' << frame.height << "\n255\n";

    std::vector<std::uint8_t> rgbRow(static_cast<std::size_t>(frame.width) * 3);

    for (std::uint32_t y = 0; y < frame.height; ++y) {
        const std::uint8_t* sourceRow =
            frame.data + static_cast<std::size_t>(y) * frame.rowStrideInBytes;

        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::uint8_t* sourcePixel = sourceRow + static_cast<std::size_t>(x) * 4;

            std::uint8_t* destinationPixel = rgbRow.data() + static_cast<std::size_t>(x) * 3;

            destinationPixel[0] = sourcePixel[2];
            destinationPixel[1] = sourcePixel[1];
            destinationPixel[2] = sourcePixel[0];
        }

        output.write(reinterpret_cast<const char*>(rgbRow.data()),
                     static_cast<std::streamsize>(rgbRow.size()));

        if (!output) {
            std::cerr << "failed while writing frame\n";
            return false;
        }
    }

    std::cout << "saved first frame: " << path << '\n';

    return true;
}

} // namespace

int main() {
    WaylandCaptureSession capture;

    std::atomic<std::uint64_t> frameCount{0};
    std::atomic<bool> frameSaved{false};

    WaylandCaptureSession::Config config;

    config.sourceType = WaylandCaptureSession::SourceType::Monitor;

    config.cursorMode = WaylandCaptureSession::CursorMode::Embedded;

    WaylandCaptureSession::Callbacks callbacks;

    callbacks.onFrame = [&frameCount, &frameSaved](const WaylandCaptureSession::Frame& frame) {
        if (!frameSaved.exchange(true)) {
            saveFrameAsPpm(frame, "captured_frame.ppm");
        }

        const std::uint64_t count = ++frameCount;

        if (count == 1 || count % 60 == 0) {
            std::cout << "frame: " << count << ", size: " << frame.width << 'x' << frame.height
                      << ", stride: " << frame.rowStrideInBytes << ", pts: " << frame.ptsNs << " ns"
                      << ", sequence: " << frame.sequence << '\n';
        }
    };

    callbacks.onStateChanged = [](WaylandCaptureSession::State state) {
        std::cout << "state: " << stateName(state) << '\n';
    };

    callbacks.onError = [](std::string_view message) {
        std::cerr << "capture error: " << message << '\n';
    };

    if (!capture.start(config, std::move(callbacks))) {
        std::cerr << "failed to start capture\n";
        return 1;
    }

    std::cout << "commands:\n"
              << "  p: pause\n"
              << "  r: resume\n"
              << "  q: quit\n";

    char command = '\0';

    while (std::cin >> command) {
        switch (command) {
        case 'p':
            capture.pause();
            break;

        case 'r':
            capture.resume();
            break;

        case 'q':
            capture.stop();

            std::cout << "total frames: " << frameCount.load() << '\n';

            return 0;

        default:
            std::cout << "unknown command: " << command << '\n';
            break;
        }
    }

    capture.stop();
    return 0;
}