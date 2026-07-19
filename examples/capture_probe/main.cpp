#include "wayland_capture_session.h"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

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
        std::cerr << "failed to open: " << path << '\n';
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

            // PipeWire 输出 BGRA/BGRx，PPM 需要 RGB。
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

    std::cout << "saved frame: " << path << '\n';

    return true;
}

int main() {
    // 注意：capture 在循环外创建。
    // 两轮测试使用的是同一个对象。
    WaylandCaptureSession capture;

    WaylandCaptureSession::Config config;
    config.sourceType = WaylandCaptureSession::SourceType::Window;
    config.cursorMode = WaylandCaptureSession::CursorMode::Embedded;

    for (int round = 1; round <= 2; ++round) {
        std::atomic<std::uint64_t> frameCount{0};
        std::atomic<bool> frameSaved{false};

        WaylandCaptureSession::Callbacks callbacks;

        callbacks.onFrame = [&frameCount, &frameSaved](const WaylandCaptureSession::Frame& frame) {
            if (!frameSaved.exchange(true)) {
                if (!saveFrameAsPpm(frame, "captured_frame.ppm")) {
                    std::cerr << "failed to save captured frame\n";
                }
            }

            const std::uint64_t count = ++frameCount;

            if (count == 1 || count % 60 == 0) {
                std::cout << "callback frame: " << count << ", size: " << frame.width << 'x'
                          << frame.height << ", pts: " << frame.ptsNs << " ns"
                          << ", sequence: " << frame.sequence << '\n';
            }
        };

        callbacks.onStateChanged = [](WaylandCaptureSession::State state) {
            std::cout << "state: " << static_cast<int>(state) << '\n';
        };

        callbacks.onError = [](std::string_view message) {
            std::cerr << "error: " << message << '\n';
        };

        std::cout << "\n=== capture round " << round << " ===\n";

        if (!capture.start(config, std::move(callbacks))) {
            std::cerr << "failed to start capture round " << round << '\n';

            return 1;
        }

        WaylandCaptureSession::Callbacks duplicateCallbacks;

        const bool duplicateStartResult = capture.start(config, std::move(duplicateCallbacks));

        std::cout << "duplicate start result: " << (duplicateStartResult ? "true" : "false")
                  << '\n';

        std::cout << "commands:\n"
                  << "  p: pause\n"
                  << "  r: resume\n"
                  << "  q: stop current round\n";

        char command = '\0';

        while (std::cin >> command) {
            if (command == 'p') {
                capture.pause();

                std::cout << "pause requested\n";

                continue;
            }

            if (command == 'r') {
                capture.resume();

                std::cout << "resume requested\n";

                continue;
            }

            if (command == 'q') {
                break;
            }

            std::cout << "unknown command: " << command << '\n';
        }

        // 即使第一轮已经因为用户取消进入 Error，
        // 仍然需要 stop() 来 join 工作线程并恢复到 Idle。
        capture.stop();

        std::cout << "first stop completed\n";

        capture.stop();

        std::cout << "second stop completed\n";

        std::cout << "round " << round << " stopped"
                  << ", total frames: " << frameCount.load() << '\n';
    }

    std::cout << "all rounds completed\n";
    return 0;
}