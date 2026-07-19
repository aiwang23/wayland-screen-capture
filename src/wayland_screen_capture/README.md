# Wayland Screen Capture

基于 XDG Desktop Portal 和 PipeWire 的 Wayland 屏幕捕获组件。

当前主要在 Debian 13、KDE Plasma Wayland 环境验证。

## 功能

* 捕获显示器或单个窗口
* 通过系统 Portal 选择捕获源
* 输出 BGRA/BGRx 原始帧
* 支持嵌入或隐藏鼠标光标
* 支持暂停、恢复和停止
* 支持窗口尺寸动态变化
* 提供帧时间戳和序列号
* 支持停止后重新启动
* 自动释放 Portal、PipeWire 和线程资源

## 依赖

需要以下开发库：

```text
gio-2.0
libportal
libpipewire-0.3
libspa-0.2
```

Debian 可安装：

```bash
sudo apt install libglib2.0-dev libportal-dev libpipewire-0.3-dev
```

组件要求 C++17。

## CMake 接入

项目根目录引入：

```cmake
add_subdirectory(
    third_party/wayland-screen-capture
)

target_link_libraries(
    your_app
    PRIVATE
    wayland_screen_capture::wayland_screen_capture
)
```

代码中包含：

```cpp
#include "wayland_capture_session.h"
```

## 最小示例

可直接复制到应用程序入口中使用：

```cpp
#include "wayland_capture_session.h"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

int main()
{
    WaylandCaptureSession capture;

    WaylandCaptureSession::Config config;
    config.sourceType =
        WaylandCaptureSession::SourceType::Monitor;
    config.cursorMode =
        WaylandCaptureSession::CursorMode::Embedded;

    WaylandCaptureSession::Callbacks callbacks;

    callbacks.onFrame =
        [](const WaylandCaptureSession::Frame& frame) {
            std::cout
                << "frame: "
                << frame.width
                << 'x'
                << frame.height
                << ", stride: "
                << frame.rowStrideInBytes
                << ", pts: "
                << frame.ptsNs
                << " ns\n";

            // frame.data 只在本次回调执行期间有效。
            // 可在这里复制数据或立即交给编码器处理。
        };

    callbacks.onStateChanged =
        [](WaylandCaptureSession::State state) {
            std::cout
                << "state: "
                << static_cast<int>(state)
                << '\n';
        };

    callbacks.onError =
        [](std::string_view message) {
            std::cerr
                << "capture error: "
                << message
                << '\n';
        };

    if (!capture.start(config, std::move(callbacks))) {
        std::cerr << "failed to start capture\n";
        return 1;
    }

    std::cout << "press Enter to stop\n";
    std::cin.get();

    capture.stop();
    return 0;
}
```

## 配置

### 捕获源

捕获整个显示器：

```cpp
config.sourceType =
    WaylandCaptureSession::SourceType::Monitor;
```

捕获单个窗口：

```cpp
config.sourceType =
    WaylandCaptureSession::SourceType::Window;
```

实际捕获目标由系统 Portal 窗口选择。

### 鼠标光标

将鼠标嵌入画面：

```cpp
config.cursorMode =
    WaylandCaptureSession::CursorMode::Embedded;
```

隐藏鼠标：

```cpp
config.cursorMode =
    WaylandCaptureSession::CursorMode::Hidden;
```

## 帧格式

`Frame::data` 固定为四字节像素布局：

```text
B G R A
```

部分环境可能输出 BGRx，第 4 个字节不应作为有效透明度使用。

相关字段：

```cpp
frame.data;
frame.width;
frame.height;
frame.rowStrideInBytes;
frame.isContiguous;
frame.ptsNs;
frame.sequence;
```

逐行访问：

```cpp
for (std::uint32_t y = 0; y < frame.height; ++y) {
    const std::uint8_t* row =
        frame.data +
        static_cast<std::size_t>(y) *
            frame.rowStrideInBytes;

    // 使用 row
}
```

不要默认：

```cpp
frame.rowStrideInBytes == frame.width * 4
```

应使用实际的 `rowStrideInBytes`，或先检查 `isContiguous`。

## 生命周期

```text
Idle
→ Starting
→ Streaming
→ Stopping
→ Idle
```

主动暂停时：

```text
Streaming
→ Paused
→ Streaming
```

接口：

```cpp
bool start(const Config& config, Callbacks callbacks);

void pause();
void resume();
void stop();

State state() const;
bool isRunning() const;
bool isPaused() const;
```

`start()` 为异步启动。返回 `true` 表示启动流程已发起，不代表已经开始输出帧。

应通过：

```cpp
callbacks.onStateChanged
```

确认状态进入 `State::Streaming`。

`pause()` 和 `resume()` 同样异步生效，最终状态以 `onStateChanged` 为准。

`stop()` 会等待内部工作线程退出。析构函数也会自动停止捕获。

## 注意事项

### 帧内存生命周期

`Frame::data` 指向 PipeWire 缓冲区，只在 `onFrame()` 回调执行期间有效。

错误用法：

```cpp
const std::uint8_t* savedData = nullptr;

callbacks.onFrame = [&](const auto& frame) {
    savedData = frame.data;
};

// 回调结束后 savedData 已失效。
```

需要异步处理时，应在回调内复制数据。

### 不要阻塞帧回调

`onFrame()` 运行在 PipeWire 线程。

不要在回调中执行：

* 长时间文件写入
* 网络阻塞发送
* 等待互斥锁
* 耗时的软件编码
* 无限队列入队

低延迟应用建议只保留最新一帧，覆盖尚未处理的旧帧。

### 生命周期调用线程

当前建议由应用主线程调用：

```cpp
start();
pause();
resume();
stop();
```

不要在以下回调内部直接调用生命周期接口：

```cpp
onFrame
onStateChanged
onError
```

### 窗口最小化

在 KDE Wayland 环境中，单窗口捕获具有以下行为：

```text
窗口可见
→ 正常输出帧

窗口最小化
→ PipeWire 暂停输出

窗口恢复
→ 自动继续输出帧
```

捕获整个显示器时不受单个窗口最小化影响。

### 静止画面帧率

KDE/KWin 可能根据画面变化动态调整输出频率：

```text
画面静止
→ 输出帧率降低

窗口滚动或画面持续变化
→ 输出帧率提高
```

不要假设 PipeWire 一定按照固定帧率持续输出。

## 当前限制

* 仅验证 KDE Plasma Wayland
* 暂未验证 GNOME 和其他 Portal 实现
* 暂未实现 DMA-BUF 零拷贝输出
* 暂未实现捕获源消失的独立状态通知
* 暂未实现 Portal 或 PipeWire 服务异常后的自动重连
* 暂未提供固定帧率输出
