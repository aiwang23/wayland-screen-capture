# Capture Probe

`wayland_screen_capture` 的测试程序，用于验证 Wayland 屏幕或窗口捕获功能。

## 功能

* 通过系统 Portal 选择显示器或窗口
* 打印捕获状态
* 打印帧尺寸、步长、时间戳和序列号
* 统计接收到的帧数
* 暂停和恢复捕获
* 验证停止、重复启动和异常恢复
* 可用于保存第一帧并检查实际画面

## 依赖

依赖项目内的：

```text
wayland_screen_capture
```

系统依赖由组件自动处理：

```text
gio-2.0
libportal
libpipewire-0.3
libspa-0.2
```

## 编译

在项目根目录执行：

```bash
cmake -S . -B build
cmake --build build
```

运行：

```bash
./build/examples/capture_probe/wayland_capture_probe
```

## 使用流程

程序启动后：

1. 系统弹出 Portal 选择窗口。
2. 选择要捕获的显示器或应用窗口。
3. 捕获成功后终端会持续打印帧信息。
4. 输入命令控制捕获状态。

命令：

```text
p：暂停捕获
r：恢复捕获
q：停止并退出
```

正常启动日志类似：

```text
state: 1
portal created
creating screencast session...
session created, starting...
session started
PipeWire core connected
video format: BGRA
video size: 2520x1680
stream state: paused -> streaming
state: 2
callback frame: 1
```

状态编号：

```text
0：Idle
1：Starting
2：Streaming
3：Paused
4：Stopping
5：Error
```

## 捕获配置

在 `main.cpp` 中修改 `Config`。

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

## 测试方法

### 基础抓帧

选择捕获源后，确认出现：

```text
state: 2
callback frame: 1
```

### 暂停和恢复

输入：

```text
p
```

预期：

```text
stream state: streaming -> paused
state: 3
```

输入：

```text
r
```

预期：

```text
stream state: paused -> streaming
state: 2
```

### 窗口尺寸变化

捕获窗口后拖动窗口边框。

预期重新输出：

```text
video size: 新宽度x新高度
buffer stride: ...
PipeWire buffer parameters updated
```

后续帧尺寸应同步更新。

### 窗口最小化

捕获单个窗口时：

```text
窗口最小化
→ PipeWire 暂停输出帧

窗口恢复
→ 自动继续输出帧
```

这是 KDE Wayland 的正常行为。

### 用户取消

在 Portal 选择窗口中点击取消。

预期：

```text
error: failed to start screencast session: Screencast canceled
state: 5
```

调用停止后应恢复：

```text
state: 4
state: 0
```

### 重复启动和停止

运行中再次调用 `start()` 应返回：

```text
false
```

连续调用两次 `stop()` 应安全返回，不应崩溃或卡死。

## 检查实际画面

示例可以在第一帧回调中保存 PPM 文件：

```text
captured_frame.ppm
```

打开：

```bash
xdg-open captured_frame.ppm
```

检查：

* 画面内容正确
* 颜色正常
* 没有上下颠倒
* 没有错行或撕裂
* 尺寸与日志一致

## 注意事项

* `Frame::data` 只在 `onFrame()` 回调期间有效。
* 不要保存 `Frame::data` 指针供回调结束后使用。
* `onFrame()` 运行在 PipeWire 线程，不要执行长时间阻塞操作。
* 静止画面时，KWin 可能主动降低输出帧率。
* 单窗口最小化后不会继续产生新帧。
* 当前示例主要用于调试，不负责视频编码或网络传输。
