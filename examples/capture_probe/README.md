# Capture Probe

`wayland_screen_capture` 的示例程序，用于验证显示器或窗口捕获。

## 功能

- 通过系统 Portal 选择捕获源
- 打印捕获状态和帧信息
- 保存第一帧为 `captured_frame.ppm`
- 支持暂停、恢复和停止
- 统计接收到的帧数

## 编译运行

在项目根目录执行：

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/examples/capture_probe/wayland_capture_probe
```

程序启动后会弹出系统选择窗口。

## 操作命令

```text
p：暂停捕获
r：恢复捕获
q：停止并退出
```

正常输出示例：

```text
state: Starting
state: Streaming
saved first frame: captured_frame.ppm
frame: 1, size: 2520x1680, stride: 10080, pts: ...
```

退出时：

```text
state: Stopping
state: Idle
total frames: ...
```

## 捕获配置

在 `main.cpp` 中修改配置。

捕获显示器：

```cpp
config.sourceType =
    WaylandCaptureSession::SourceType::Monitor;
```

捕获单个窗口：

```cpp
config.sourceType =
    WaylandCaptureSession::SourceType::Window;
```

嵌入鼠标：

```cpp
config.cursorMode =
    WaylandCaptureSession::CursorMode::Embedded;
```

隐藏鼠标：

```cpp
config.cursorMode =
    WaylandCaptureSession::CursorMode::Hidden;
```

## 检查捕获画面

程序会保存第一帧：

```text
captured_frame.ppm
```

打开图片：

```bash
xdg-open captured_frame.ppm
```

检查：

- 画面内容正确
- 颜色正常
- 没有上下颠倒
- 没有错行或撕裂
- 图片尺寸与日志一致

## 测试方法

### 暂停和恢复

输入：

```text
p
```

预期状态变为：

```text
state: Paused
```

输入：

```text
r
```

预期状态恢复：

```text
state: Streaming
```

### 窗口尺寸变化

捕获窗口后调整窗口大小。

日志应重新输出分辨率，后续帧尺寸也应同步变化。

### 窗口最小化

在 KDE Wayland 环境中：

```text
窗口最小化
→ 暂停输出帧

窗口恢复
→ 自动继续输出帧
```

### 取消选择

在 Portal 中取消捕获时，程序会进入 `Error` 状态。

输入 `q` 后应正常退出。

## 注意事项

- `Frame::data` 只在帧回调执行期间有效。
- 示例保存的是第一帧，不是视频文件。
- 静止画面时，KWin 可能降低输出帧率。
- 单窗口最小化后可能不再产生新帧。
- 当前示例只用于验证抓屏，不包含视频编码和网络传输。
