# Wayland Screen Capture

基于 XDG Desktop Portal 和 PipeWire 的 Wayland 屏幕捕获组件。

使用 C++17 编写，不依赖 Qt，可通过 CMake 或 Git 子模块接入其他项目。

## 当前功能

* 捕获显示器或单个窗口
* 通过系统 Portal 选择捕获源
* 输出 BGRA/BGRx 原始帧
* 支持嵌入或隐藏鼠标
* 提供帧尺寸、步长、PTS 和序列号
* 支持窗口尺寸动态变化
* 支持暂停、恢复、停止和重新启动
* 自动管理 Portal、PipeWire 和工作线程资源

## 当前状态

已在以下环境验证：

```text
Debian 13
KDE Plasma Wayland
libportal 0.9.1
PipeWire 1.4.2
GCC / C++17
```

当前适合用于：

```text
桌面同屏
屏幕录像
远程桌面
视频编码输入
WebRTC / SRT 推流
```

## 安装依赖

Debian：

```bash
sudo apt update && sudo apt install -y build-essential cmake ninja-build pkg-config libglib2.0-dev libportal-dev libpipewire-0.3-dev
```

## 编译运行

```bash
git clone https://github.com/aiwang23/wayland-screen-capture.git
cd wayland-screen-capture
cmake -S . -B build -G Ninja
cmake --build build
./build/examples/capture_probe/wayland_capture_probe
```

程序启动后会弹出系统 Portal 窗口，用于选择显示器或应用窗口。

## 作为子模块接入

```bash
git submodule add https://github.com/aiwang23/wayland-screen-capture.git third_party/wayland-screen-capture
```

项目 `CMakeLists.txt`：

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

完整接口和最小示例见组件文档。

## 关键配置

捕获显示器：

```cpp
config.sourceType =
    WaylandCaptureSession::SourceType::Monitor;
```

捕获窗口：

```cpp
config.sourceType =
    WaylandCaptureSession::SourceType::Window;
```

鼠标模式：

```cpp
config.cursorMode =
    WaylandCaptureSession::CursorMode::Embedded;
```

或：

```cpp
config.cursorMode =
    WaylandCaptureSession::CursorMode::Hidden;
```

## 项目结构

```text
wayland-screen-capture/
├── CMakeLists.txt
├── README.md
├── examples/
│   └── capture_probe/
│       ├── CMakeLists.txt
│       ├── README.md
│       └── main.cpp
└── src/
    └── wayland_screen_capture/
        ├── CMakeLists.txt
        ├── README.md
        ├── wayland_capture_session.cpp
        └── wayland_capture_session.h
```

## 文档

* [组件接入与接口说明](src/wayland_screen_capture/README.md)
* [Capture Probe 使用和测试](examples/capture_probe/README.md)

## 当前限制

* 目前主要验证 KDE Plasma Wayland
* 暂未验证 GNOME 和其他 Portal 实现
* 暂未支持 DMA-BUF 零拷贝
* 暂未实现 Portal 或 PipeWire 异常后的自动重连
* 单窗口最小化后，KDE 会暂停帧输出
