#include "wayland_capture_session.h"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <exception>
#include <gio/gio.h>
#include <glib.h>
#include <iostream>
#include <libportal/portal.h>
#include <mutex>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/param/video/raw-types.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/vararg.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {

const char* memoryTypeName(std::uint32_t type) {
    switch (type) {
    case SPA_DATA_MemPtr:
        return "MemPtr";

    case SPA_DATA_MemFd:
        return "MemFd";

    case SPA_DATA_DmaBuf:
        return "DmaBuf";

    default:
        return "Unknown";
    }
}

} // namespace

class WaylandCaptureSession::Impl {
public:
    Impl() = default;

    ~Impl() {
        stop();
    }

    bool start(const Config& config, Callbacks callbacks) {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);

            if (state_.load() != State::Idle) {
                return false;
            }

            config_ = config;
            callbacks_ = std::move(callbacks);
            stopRequested_.store(false);
            pauseRequested_.store(false);
            state_.store(State::Starting);
        }

        notifyStateChanged(State::Starting);

        try {
            workerThread_ = std::thread(&Impl::run, this);
        } catch (const std::exception& exception) {
            reportError(exception.what());
            return false;
        }

        return true;
    }

    void stop() {
        State currentState = state_.load();

        if (currentState == State::Idle) {
            return;
        }

        stopRequested_.store(true);

        if (currentState != State::Stopping) {
            setState(State::Stopping);
        }

        GMainLoop* mainLoop = nullptr;
        GCancellable* cancellable = nullptr;

        {
            std::lock_guard<std::mutex> lock(resourceMutex_);

            if (mainLoop_ != nullptr) {
                mainLoop = g_main_loop_ref(mainLoop_);
            }

            if (cancellable_ != nullptr) {
                cancellable = G_CANCELLABLE(g_object_ref(cancellable_));
            }
        }

        if (cancellable != nullptr) {
            g_cancellable_cancel(cancellable);
            g_object_unref(cancellable);
        }

        if (mainLoop != nullptr) {
            g_main_loop_quit(mainLoop);
            g_main_loop_unref(mainLoop);
        }

        if (workerThread_.joinable()) {
            workerThread_.join();
        }

        StateCallback idleCallback;

        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);

            state_.store(State::Idle);
            idleCallback = callbacks_.onStateChanged;
            callbacks_ = {};
        }

        if (idleCallback) {
            idleCallback(State::Idle);
        }
    }

    void pause() {
        if (state_.load() != State::Streaming) {
            return;
        }

        int result = 0;

        {
            std::lock_guard<std::mutex> lock(pipewireMutex_);

            // 获取锁后重新检查，防止等待锁期间已经开始停止。
            if (stopRequested_.load() || state_.load() != State::Streaming ||
                pipewireLoop_ == nullptr || stream_ == nullptr) {
                return;
            }

            pauseRequested_.store(true);

            pw_thread_loop_lock(pipewireLoop_);

            result = pw_stream_set_active(stream_, false);

            pw_thread_loop_unlock(pipewireLoop_);

            if (result < 0) {
                pauseRequested_.store(false);
            }
        }

        if (result < 0) {
            reportError("failed to pause PipeWire stream");
        }
    }

    void resume() {
        if (state_.load() != State::Paused) {
            return;
        }

        int result = 0;

        {
            std::lock_guard<std::mutex> lock(pipewireMutex_);

            // 获取锁后重新检查，防止等待期间已经停止。
            if (stopRequested_.load() || state_.load() != State::Paused ||
                pipewireLoop_ == nullptr || stream_ == nullptr) {
                return;
            }

            pauseRequested_.store(false);

            pw_thread_loop_lock(pipewireLoop_);

            result = pw_stream_set_active(stream_, true);

            pw_thread_loop_unlock(pipewireLoop_);

            if (result < 0) {
                // 恢复失败，实际仍处于暂停状态。
                pauseRequested_.store(true);
            }
        }

        if (result < 0) {
            reportError("failed to resume PipeWire stream");
        }
    }

    bool isPaused() const {
        return state_.load() == State::Paused;
    }

    State state() const {
        return state_.load();
    }

    bool isRunning() const {
        const State currentState = state_.load();

        return currentState == State::Starting || currentState == State::Streaming ||
               currentState == State::Paused;
    }

private:
    void run() {
        GMainContext* mainContext = g_main_context_new();

        if (mainContext == nullptr) {
            reportError("failed to create GLib main context");
            return;
        }

        GMainLoop* mainLoop = g_main_loop_new(mainContext, FALSE);

        if (mainLoop == nullptr) {
            g_main_context_unref(mainContext);

            reportError("failed to create GLib main loop");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(resourceMutex_);
            mainContext_ = mainContext;
            mainLoop_ = mainLoop;
        }

        g_main_context_push_thread_default(mainContext);

        GError* error = nullptr;

        portal_ = xdp_portal_initable_new(&error);

        if (portal_ == nullptr) {
            std::string message = "failed to create XDG Desktop Portal";

            if (error != nullptr) {
                message += ": ";
                message += error->message;
                g_error_free(error);
            }

            reportError(message);
        } else {
            std::cout << "portal created\n";

            GCancellable* cancellable = g_cancellable_new();

            if (cancellable == nullptr) {
                reportError("failed to create cancellable");
            } else {
                {
                    std::lock_guard<std::mutex> lock(resourceMutex_);
                    cancellable_ = cancellable;
                }

                const auto outputType = config_.sourceType == SourceType::Window
                                            ? XDP_OUTPUT_WINDOW
                                            : XDP_OUTPUT_MONITOR;

                const auto cursorMode = config_.cursorMode == CursorMode::Hidden
                                            ? XDP_CURSOR_MODE_HIDDEN
                                            : XDP_CURSOR_MODE_EMBEDDED;

                std::cout << "creating screencast session...\n";

                xdp_portal_create_screencast_session(portal_, outputType, XDP_SCREENCAST_FLAG_NONE,
                                                     cursorMode, XDP_PERSIST_MODE_NONE, nullptr,
                                                     cancellable, &Impl::onSessionCreated, this);

                if (!stopRequested_.load()) {
                    g_main_loop_run(mainLoop);
                }
            }
        }

        stopPipeWire();

        if (session_ != nullptr) {
            g_object_unref(session_);
            session_ = nullptr;

            std::cout << "session destroyed\n";
        }

        GCancellable* cancellable = nullptr;

        {
            std::lock_guard<std::mutex> lock(resourceMutex_);

            cancellable = cancellable_;
            cancellable_ = nullptr;
        }

        if (cancellable != nullptr) {
            g_object_unref(cancellable);
        }
        if (portal_ != nullptr) {
            g_object_unref(portal_);
            portal_ = nullptr;

            std::cout << "portal destroyed\n";
        }

        g_main_context_pop_thread_default(mainContext);

        {
            std::lock_guard<std::mutex> lock(resourceMutex_);
            mainLoop_ = nullptr;
            mainContext_ = nullptr;
        }

        g_main_loop_unref(mainLoop);
        g_main_context_unref(mainContext);
    }

    void setState(State newState) {
        state_.store(newState);
        notifyStateChanged(newState);
    }

    void notifyStateChanged(State newState) {
        const StateCallback callback = callbacks_.onStateChanged;

        if (callback) {
            callback(newState);
        }
    }

    void reportError(std::string_view message) {
        state_.store(State::Error);

        const ErrorCallback errorCallback = callbacks_.onError;

        if (errorCallback) {
            errorCallback(message);
        }

        notifyStateChanged(State::Error);

        GMainLoop* mainLoop = nullptr;

        {
            std::lock_guard<std::mutex> lock(resourceMutex_);

            if (mainLoop_ != nullptr) {
                mainLoop = g_main_loop_ref(mainLoop_);
            }
        }

        if (mainLoop != nullptr) {
            g_main_loop_quit(mainLoop);
            g_main_loop_unref(mainLoop);
        }
    }

    static void onSessionCreated(GObject* sourceObject, GAsyncResult* result, gpointer userData) {
        auto* self = static_cast<Impl*>(userData);

        GError* error = nullptr;

        XdpSession* session =
            xdp_portal_create_screencast_session_finish(XDP_PORTAL(sourceObject), result, &error);

        if (session == nullptr) {
            if (self->stopRequested_.load() && error != nullptr &&
                g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_error_free(error);
                return;
            }

            std::string message = "failed to create screencast session";

            if (error != nullptr) {
                message += ": ";
                message += error->message;
                g_error_free(error);
            }

            self->reportError(message);
            return;
        }

        if (self->stopRequested_.load()) {
            g_object_unref(session);
            return;
        }

        self->session_ = session;

        std::cout << "session created, starting...\n";

        GCancellable* cancellable = nullptr;

        {
            std::lock_guard<std::mutex> lock(self->resourceMutex_);

            if (self->cancellable_ != nullptr) {
                cancellable = G_CANCELLABLE(g_object_ref(self->cancellable_));
            }
        }

        xdp_session_start(session, nullptr, cancellable, &Impl::onSessionStarted, self);

        if (cancellable != nullptr) {
            g_object_unref(cancellable);
        }
    }

    static void onSessionStarted(GObject* sourceObject, GAsyncResult* result, gpointer userData) {
        auto* self = static_cast<Impl*>(userData);

        GError* error = nullptr;

        if (!xdp_session_start_finish(XDP_SESSION(sourceObject), result, &error)) {
            if (self->stopRequested_.load() && error != nullptr &&
                g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                g_error_free(error);
                return;
            }

            std::string message = "failed to start screencast session";

            if (error != nullptr) {
                message += ": ";
                message += error->message;
                g_error_free(error);
            }

            self->reportError(message);
            return;
        }

        std::cout << "session started\n";

        GVariant* streams = xdp_session_get_streams(XDP_SESSION(sourceObject));

        if (streams == nullptr || g_variant_n_children(streams) == 0) {
            if (streams != nullptr) {
                g_variant_unref(streams);
            }

            self->reportError("no stream selected");
            return;
        }

        const gsize streamCount = g_variant_n_children(streams);

        std::cout << "stream count: " << streamCount << '\n';

        std::uint32_t nodeId = 0;
        GVariant* properties = nullptr;

        g_variant_get_child(streams, 0, "(u@a{sv})", &nodeId, &properties);

        self->nodeId_ = nodeId;

        std::cout << "stream node id: " << nodeId << '\n';
        const int pipewireFd = xdp_session_open_pipewire_remote(XDP_SESSION(sourceObject));

        if (pipewireFd < 0) {
            if (properties != nullptr) {
                g_variant_unref(properties);
            }

            g_variant_unref(streams);

            self->reportError("failed to open PipeWire remote");
            return;
        }

        std::cout << "PipeWire remote fd: " << pipewireFd << '\n';

        if (!self->startPipeWireRemote(pipewireFd)) {
            if (properties != nullptr) {
                g_variant_unref(properties);
            }

            g_variant_unref(streams);

            self->reportError("failed to connect PipeWire remote");
            return;
        }

        if (!self->startPipeWireStream(nodeId)) {
            if (properties != nullptr) {
                g_variant_unref(properties);
            }

            g_variant_unref(streams);

            self->reportError("failed to create PipeWire stream");
            return;
        }

        if (properties != nullptr) {
            g_variant_unref(properties);
        }

        g_variant_unref(streams);
    }
    
    void stopPipeWire() {
        std::lock_guard<std::mutex> lock(pipewireMutex_);

        if (pipewireLoopStarted_) {
            pw_thread_loop_stop(pipewireLoop_);
            pipewireLoopStarted_ = false;
        }

        if (stream_ != nullptr) {
            pw_stream_destroy(stream_);
            stream_ = nullptr;
        }

        if (pipewireCore_ != nullptr) {
            pw_core_disconnect(pipewireCore_);
            pipewireCore_ = nullptr;
        }

        if (pipewireContext_ != nullptr) {
            pw_context_destroy(pipewireContext_);
            pipewireContext_ = nullptr;
        }

        if (pipewireLoop_ != nullptr) {
            pw_thread_loop_destroy(pipewireLoop_);
            pipewireLoop_ = nullptr;
        }

        if (pipewireInitialized_) {
            pw_deinit();
            pipewireInitialized_ = false;
        }

        formatReady_.store(false);
        videoFormat_ = 0;
        width_ = 0;
        height_ = 0;

        firstFrameLogged_.store(false);
        pauseRequested_.store(false);
    }

    bool startPipeWireRemote(int pipewireFd) {
        pw_init(nullptr, nullptr);
        pipewireInitialized_ = true;

        pipewireLoop_ = pw_thread_loop_new("wayland_capture_session", nullptr);

        if (pipewireLoop_ == nullptr) {
            std::cerr << "pw_thread_loop_new failed\n";

            close(pipewireFd);
            stopPipeWire();
            return false;
        }

        pipewireContext_ = pw_context_new(pw_thread_loop_get_loop(pipewireLoop_), nullptr, 0);

        if (pipewireContext_ == nullptr) {
            std::cerr << "pw_context_new failed: " << std::strerror(errno) << '\n';

            close(pipewireFd);
            stopPipeWire();
            return false;
        }

        pipewireCore_ = pw_context_connect_fd(pipewireContext_, pipewireFd, nullptr, 0);

        if (pipewireCore_ == nullptr) {
            std::cerr << "pw_context_connect_fd failed: " << std::strerror(errno) << '\n';

            // connect_fd 已经接管 fd；
            // 失败时也会自动关闭，不能再次 close。
            stopPipeWire();
            return false;
        }

        if (pw_thread_loop_start(pipewireLoop_) < 0) {
            std::cerr << "pw_thread_loop_start failed: " << std::strerror(errno) << '\n';

            stopPipeWire();
            return false;
        }

        pipewireLoopStarted_ = true;

        std::cout << "PipeWire core connected\n";

        return true;
    }

    bool startPipeWireStream(std::uint32_t nodeId) {
        if (pipewireLoop_ == nullptr || pipewireCore_ == nullptr) {
            return false;
        }

        pw_thread_loop_lock(pipewireLoop_);

        stream_ = pw_stream_new(pipewireCore_, "wayland_capture_session",
                                pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                                  "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));

        if (stream_ == nullptr) {
            pw_thread_loop_unlock(pipewireLoop_);

            std::cerr << "pw_stream_new failed\n";
            return false;
        }

        streamEvents_ = {};
        streamEvents_.version = PW_VERSION_STREAM_EVENTS;
        streamEvents_.state_changed = &Impl::onStreamStateChanged;
        streamEvents_.param_changed = &Impl::onStreamParamChanged;
        streamEvents_.process = &Impl::onStreamProcess;

        pw_stream_add_listener(stream_, &streamListener_, &streamEvents_, this);

        std::uint8_t podBuffer[1024];

        spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

        const spa_pod* params[1];

        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,

            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),

            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),

            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(2, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA)));

        const auto flags =
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);

        const int result = pw_stream_connect(stream_, PW_DIRECTION_INPUT, nodeId, flags, params, 1);

        if (result < 0) {
            std::cerr << "pw_stream_connect failed: " << result << '\n';

            pw_stream_destroy(stream_);
            stream_ = nullptr;

            pw_thread_loop_unlock(pipewireLoop_);
            return false;
        }

        pw_thread_loop_unlock(pipewireLoop_);

        std::cout << "PipeWire stream connected\n";
        return true;
    }

    static void onStreamStateChanged(void* userData, pw_stream_state oldState,
                                     pw_stream_state newState, const char* error) {
        auto* self = static_cast<Impl*>(userData);

        std::cout << "stream state: " << pw_stream_state_as_string(oldState) << " -> "
                  << pw_stream_state_as_string(newState) << '\n';

        if (newState == PW_STREAM_STATE_ERROR) {
            std::string message = "PipeWire stream error";

            if (error != nullptr) {
                message += ": ";
                message += error;
            }

            self->reportError(message);
            return;
        }

        if (newState == PW_STREAM_STATE_PAUSED && self->pauseRequested_.load() &&
            self->state_.load() == State::Streaming) {
            self->setState(State::Paused);
            return;
        }

        if (newState == PW_STREAM_STATE_STREAMING) {
            const State currentState = self->state_.load();

            if (currentState == State::Starting || currentState == State::Paused) {
                self->pauseRequested_.store(false);
                self->setState(State::Streaming);
            }
        }
    }

    static void onStreamParamChanged(void* userData, std::uint32_t id, const spa_pod* param) {
        auto* self = static_cast<Impl*>(userData);

        if (id != SPA_PARAM_Format || param == nullptr) {
            return;
        }

        spa_video_info_raw format{};

        if (spa_format_video_raw_parse(param, &format) < 0) {
            self->reportError("failed to parse PipeWire video format");
            return;
        }

        const char* formatName = spa_type_video_format_to_short_name(format.format);

        std::cout << "video format: " << (formatName != nullptr ? formatName : "unknown") << '\n';

        std::cout << "video size: " << format.size.width << 'x' << format.size.height << '\n';

        const std::uint32_t stride = format.size.width * 4;

        const std::uint32_t bufferSize = stride * format.size.height;

        std::uint8_t podBuffer[1024];

        spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

        const std::int32_t memoryTypes = (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd);

        const spa_pod* params[2];

        params[0] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,

            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),

            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),

            SPA_PARAM_BUFFERS_size, SPA_POD_Int(static_cast<std::int32_t>(bufferSize)),

            SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<std::int32_t>(stride)),

            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(memoryTypes)));

        params[1] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,

            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),

            SPA_PARAM_META_size, SPA_POD_Int(static_cast<std::int32_t>(sizeof(spa_meta_header)))));

        const int result = pw_stream_update_params(self->stream_, params, 2);

        if (result < 0) {
            self->reportError("failed to update PipeWire buffer parameters");
            return;
        }

        self->videoFormat_ = format.format;
        self->width_ = format.size.width;
        self->height_ = format.size.height;
        self->formatReady_.store(true);

        std::cout << "buffer stride: " << stride << '\n';

        std::cout << "buffer size: " << bufferSize << '\n';

        std::cout << "PipeWire buffer parameters updated\n";
    }

    static void onStreamProcess(void* userData) {
        auto* self = static_cast<Impl*>(userData);

        pw_buffer* pipewireBuffer = pw_stream_dequeue_buffer(self->stream_);

        if (pipewireBuffer == nullptr) {
            return;
        }

        // 无论后面是否成功处理，都必须归还。
        const auto queueBuffer = [&]() { pw_stream_queue_buffer(self->stream_, pipewireBuffer); };

        if (!self->formatReady_.load()) {
            queueBuffer();
            return;
        }

        spa_buffer* buffer = pipewireBuffer->buffer;

        if (buffer == nullptr || buffer->n_datas == 0) {
            std::cerr << "received buffer without data\n";

            queueBuffer();
            return;
        }

        const spa_data& data = buffer->datas[0];

        if (data.data == nullptr || data.chunk == nullptr) {
            std::cerr << "received unmapped buffer, memory: " << memoryTypeName(data.type) << '\n';

            queueBuffer();
            return;
        }

        const std::int32_t stride = data.chunk->stride;

        if (stride <= 0 || data.maxsize == 0) {
            std::cerr << "invalid frame stride or buffer size\n";

            queueBuffer();
            return;
        }

        const std::uint32_t offset = data.chunk->offset;

        const std::size_t requiredBytes = static_cast<std::size_t>(stride) * self->height_;

        if (offset > data.maxsize || data.chunk->size < requiredBytes ||
            requiredBytes > data.maxsize - offset) {
            std::cerr << "invalid frame memory range"
                      << ", chunk size: " << data.chunk->size << ", required: " << requiredBytes
                      << ", offset: " << offset << ", max size: " << data.maxsize << '\n';

            queueBuffer();
            return;
        }

        const auto* header = static_cast<const spa_meta_header*>(
            spa_buffer_find_meta_data(buffer, SPA_META_Header, sizeof(spa_meta_header)));

        if (!self->firstFrameLogged_.exchange(true)) {
            std::cout << "first frame received\n";

            std::cout << "memory: " << memoryTypeName(data.type) << '\n';

            std::cout << "stride: " << stride << '\n';

            std::cout << "buffer size: " << data.chunk->size << '\n';

            std::cout << "buffer offset: " << data.chunk->offset << '\n';

            std::cout << "buffer max size: " << data.maxsize << '\n';

            std::cout << "contiguous: "
                      << (stride == static_cast<std::int32_t>(self->width_ * 4) ? "yes" : "no")
                      << '\n';

            if (header != nullptr) {
                std::cout << "timestamp: " << header->pts << " ns\n";

                std::cout << "sequence: " << header->seq << '\n';
            } else {
                std::cout << "timestamp metadata: unavailable\n";
            }
        }

        const auto* source = static_cast<const std::uint8_t*>(data.data) + offset;

        Frame frame;

        frame.data = source;
        frame.width = self->width_;
        frame.height = self->height_;
        frame.rowStrideInBytes = stride;
        frame.isContiguous = stride == static_cast<std::int32_t>(self->width_ * 4);
        if (header != nullptr) {
            frame.ptsNs = header->pts;
            frame.sequence = header->seq;
        }

        const FrameCallback callback = self->callbacks_.onFrame;

        if (callback) {
            try {
                callback(frame);
            } catch (const std::exception& exception) {
                std::cerr << "frame callback exception: " << exception.what() << '\n';
            } catch (...) {
                std::cerr << "frame callback unknown exception\n";
            }
        }

        queueBuffer();
    }

    Config config_;
    Callbacks callbacks_;

    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stopRequested_{false};

    std::mutex lifecycleMutex_;
    std::mutex resourceMutex_;
    std::mutex pipewireMutex_;

    std::thread workerThread_;

    GMainContext* mainContext_ = nullptr;
    GMainLoop* mainLoop_ = nullptr;
    XdpPortal* portal_ = nullptr;
    XdpSession* session_ = nullptr;
    GCancellable* cancellable_ = nullptr;
    std::uint32_t nodeId_ = 0;

    pw_thread_loop* pipewireLoop_ = nullptr;
    pw_context* pipewireContext_ = nullptr;
    pw_core* pipewireCore_ = nullptr;

    pw_stream* stream_ = nullptr;

    spa_hook streamListener_{};
    pw_stream_events streamEvents_{};
    std::uint32_t videoFormat_ = 0;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::atomic<bool> formatReady_{false};
    std::atomic<bool> firstFrameLogged_{false};

    std::atomic<bool> pauseRequested_{false};

    bool pipewireInitialized_ = false;
    bool pipewireLoopStarted_ = false;
};

WaylandCaptureSession::WaylandCaptureSession() : impl_(std::make_unique<Impl>()) {}

WaylandCaptureSession::~WaylandCaptureSession() = default;

bool WaylandCaptureSession::start(const Config& config, Callbacks callbacks) {
    return impl_->start(config, std::move(callbacks));
}

void WaylandCaptureSession::stop() {
    impl_->stop();
}

void WaylandCaptureSession::pause() {
    impl_->pause();
}

void WaylandCaptureSession::resume() {
    impl_->resume();
}

bool WaylandCaptureSession::isPaused() const {
    return impl_->isPaused();
}

WaylandCaptureSession::State WaylandCaptureSession::state() const {
    return impl_->state();
}

bool WaylandCaptureSession::isRunning() const {
    return impl_->isRunning();
}
