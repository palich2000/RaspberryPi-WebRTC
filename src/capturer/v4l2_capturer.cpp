#include "v4l2_capturer.h"

// Linux
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <linux/videodev2.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/select.h>
#include <thread>

// WebRTC
#include <modules/video_capture/video_capture_factory.h>
#include <third_party/libyuv/include/libyuv.h>

#include "common/logging.h"
#include "yuyv_clock.h"

std::shared_ptr<V4L2Capturer> V4L2Capturer::Create(Args args) {
    auto ptr = std::make_shared<V4L2Capturer>(args);
    // Patiently wait for a usable camera instead of crashing the process. The
    // bridge re-enumerates clean and broken in turns (corrupt UVC descriptor ->
    // bogus frame size); on a broken pass Initialize() returns false and we just
    // back off and try again, grabbing the device the moment it comes back clean.
    int backoff_s = kInitBackoffMinSec;
    while (!ptr->Initialize()) {
        INFO_PRINT("Camera not ready (no node / bad enumeration) - retrying in %ds.", backoff_s);
        std::this_thread::sleep_for(std::chrono::seconds(backoff_s));
        backoff_s = std::min(backoff_s * 2, kInitBackoffMaxSec);
    }
    ptr->StartCapture();
    return ptr;
}

V4L2Capturer::V4L2Capturer(Args args)
    : camera_id_(args.camera_id),
      fd_(-1),
      fps_(args.fps),
      width_(args.width),
      height_(args.height),
      rotation_(args.rotation),
      buffer_count_(4),
      hw_accel_(args.hw_accel),
      has_first_keyframe_(false),
      draw_clock_(!args.no_clock),
      format_(args.format),
      config_(args),
      capture_failure_count_(0),
      dropped_frame_count_(0) {}

V4L2Capturer::~V4L2Capturer() {
    worker_.reset();
    decoder_.reset();
    V4L2Util::StreamOff(fd_, capture_.type);
    V4L2Util::DeallocateBuffer(fd_, &capture_);
    V4L2Util::CloseDevice(fd_);
}

void V4L2Capturer::CloseFd() {
    if (fd_ >= 0) {
        V4L2Util::CloseDevice(fd_);
        fd_ = -1;
    }
}

bool V4L2Capturer::Initialize() {
    if (!hw_accel_ && format_ == V4L2_PIX_FMT_H264) {
        // Genuine config error, not a transient device state - keep it fatal.
        INFO_PRINT("Software decoding H264 camera source is not supported.");
        exit(EXIT_FAILURE);
    }

    std::string devicePath = "/dev/video" + std::to_string(camera_id_);
    fd_ = V4L2Util::TryOpenDevice(devicePath.c_str());
    if (fd_ < 0) {
        // The configured node is gone — commonly because a USB re-enumeration
        // moved the camera (e.g. video0 -> video1 after a detach/reconnect).
        // Scan for the USB capture device instead of failing outright.
        INFO_PRINT("Unable to open %s, scanning for a USB capture device...",
                   devicePath.c_str());
        fd_ = FindUsbCaptureDevice();
    }
    if (fd_ < 0) {
        INFO_PRINT("No USB capture device present yet");
        return false; // recoverable: caller backs off and retries
    }

    if (!V4L2Util::InitBuffer(fd_, &capture_, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP)) {
        ERROR_PRINT("Could not setup v4l2 capture buffer");
        CloseFd();
        return false;
    }

    if (format_ == V4L2_PIX_FMT_H264) {
        if (!SetControls(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR)) {
            ERROR_PRINT("Unable to set VBR mode");
        }
        if (!SetControls(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH)) {
            ERROR_PRINT("Unable to set H264 profile");
        }
        if (!SetControls(V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, true)) {
            ERROR_PRINT("Unable to set repeat seq header");
        }
        if (!SetControls(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_4_0)) {
            ERROR_PRINT("Unable to set H264 level");
        }
        if (!SetControls(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, 60)) {
            ERROR_PRINT("Unable to set H264 I-frame period");
        }
        if (!SetControls(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1)) {
            ERROR_PRINT("Unable to force set to key frame");
        }
    }

    if (!V4L2Util::SetFps(fd_, capture_.type, fps_)) {
        ERROR_PRINT("Unable to set fps");
    }

    if (!V4L2Util::SetCtrl(fd_, V4L2_CID_ROTATE, rotation_)) {
        ERROR_PRINT("Unable to set the rotation angle");
    }

    // SetFormat throws on a driver/input size mismatch (a corrupt re-enumeration
    // can report bogus dimensions). Treat that as a recoverable bad enumeration.
    try {
        if (!V4L2Util::SetFormat(fd_, &capture_, width_, height_, format_)) {
            ERROR_PRINT("Unable to set the resolution: %dx%d - will retry", width_, height_);
            CloseFd();
            return false;
        }
    } catch (const std::exception &e) {
        ERROR_PRINT("SetFormat failed (%s) - bad USB enumeration, will retry", e.what());
        CloseFd();
        return false;
    }

    // Reject a bogus negotiated frame size BEFORE StartCapture()'s REQBUFS. A
    // corrupt UVC descriptor (seen on bridge re-enumeration) yields a garbage
    // dwMaxVideoFrameSize, and REQBUFS then tries to vmalloc ~2 GiB per buffer
    // -> ENOMEM -> crash loop. 640x480 YUYV is 614400 B; anything past w*h*4 is
    // impossible for our formats and means the enumeration is broken.
    uint32_t image_size = V4L2Util::GetCaptureImageSize(fd_, capture_.type);
    uint64_t sane_max = (uint64_t)width_ * height_ * 4;
    if (image_size == 0 || (uint64_t)image_size > sane_max) {
        ERROR_PRINT("Negotiated frame size %u looks bogus (sane max %llu for %dx%d) - "
                    "corrupt USB descriptor from a bad re-enumeration; will retry",
                    image_size, (unsigned long long)sane_max, width_, height_);
        CloseFd();
        return false;
    }

    if (!SetControls(V4L2_CID_MPEG_VIDEO_BITRATE, 10 * 1024 * 1024)) {
        ERROR_PRINT("Unable to set video bitrate");
    }

    return true;
}

int V4L2Capturer::fps() const { return fps_; }

int V4L2Capturer::width(int stream_idx) const { return width_; }

int V4L2Capturer::height(int stream_idx) const { return height_; }

bool V4L2Capturer::is_dma_capture() const { return hw_accel_ && IsCompressedFormat(); }

uint32_t V4L2Capturer::format() const { return format_; }

Args V4L2Capturer::config() const { return config_; }

bool V4L2Capturer::IsCompressedFormat() const {
    return format_ == V4L2_PIX_FMT_MJPEG || format_ == V4L2_PIX_FMT_H264;
}

bool V4L2Capturer::CheckMatchingDevice(std::string unique_name) {
    struct v4l2_capability cap;
    if (V4L2Util::QueryCapabilities(fd_, &cap) && cap.bus_info[0] != 0 &&
        strcmp((const char *)cap.bus_info, unique_name.c_str()) == 0) {
        return true;
    }
    return false;
}

int V4L2Capturer::GetCameraIndex(webrtc::VideoCaptureModule::DeviceInfo *device_info) {
    for (int i = 0; i < device_info->NumberOfDevices(); i++) {
        char device_name[256];
        char unique_name[256];
        if (device_info->GetDeviceName(static_cast<uint32_t>(i), device_name, sizeof(device_name),
                                       unique_name, sizeof(unique_name)) == 0 &&
            CheckMatchingDevice(unique_name)) {
            DEBUG_PRINT("GetDeviceName(%d): device_name=%s, unique_name=%s", i, device_name,
                        unique_name);
            return i;
        }
    }
    return -1;
}

static inline uint8_t clamp_u8(int v) {
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static inline void rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t *Y, uint8_t *U, uint8_t *V) {
    int y = (77 * r + 150 * g + 29 * b) >> 8;          // ~0.299,0.587,0.114
    int u = ((-43 * r - 85 * g + 128 * b) >> 8) + 128; // ~-0.169,-0.331,+0.5
    int v = ((128 * r - 107 * g - 21 * b) >> 8) + 128; // ~+0.5,-0.419,-0.081
    *Y = clamp_u8(y);
    *U = clamp_u8(u);
    *V = clamp_u8(v);
}

static inline void yuyv_put_pixel(uint8_t *buf, int width, int height, int x, int y, uint8_t Y,
                                  uint8_t U, uint8_t V) {
    if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height)
        return;

    int pair_x = x & ~1; // начало пары (четный x)
    int pair_index = (y * width + pair_x) * 2;

    // Запишем U и V для пары
    buf[pair_index + 1] = U; // U0
    buf[pair_index + 3] = V; // V0

    // Запишем яркость для нужного пикселя
    if ((x & 1) == 0)
        buf[pair_index + 0] = Y; // Y0
    else
        buf[pair_index + 2] = Y; // Y1
}

static void yuyv_fill_rect_rgb(uint8_t *buf, int w, int h, int x0, int y0, int rw, int rh,
                               uint8_t r, uint8_t g, uint8_t b) {
    uint8_t Y, U, V;
    rgb_to_yuv(r, g, b, &Y, &U, &V);
    for (int y = y0; y < y0 + rh; y++) {
        for (int x = x0; x < x0 + rw; x++) {
            yuyv_put_pixel(buf, w, h, x, y, Y, U, V);
        }
    }
}

void V4L2Capturer::DrawDebugInfo(void *buffer) {
    yuv422_fmt_t fmt = static_cast<yuv422_fmt_t>(format_);
    // const int bar_h = 40;
    // const uint8_t bars[8][3] = {
    //     {255,255,255}, // white
    //     {255,255,  0}, // yellow
    //     {  0,255,255}, // cyan
    //     {  0,255,  0}, // green
    //     {255,  0,255}, // magenta
    //     {255,  0,  0}, // red
    //     {  0,  0,255}, // blue
    //     {  0,  0,  0}, // black
    // };
    // for (int i = 0; i < 8; i++) {
    //     int x0 = (width_ * i) / 8;
    //     int x1 = (width_ * (i+1)) / 8;
    //     yuyv_fill_rect_rgb(static_cast<uint8_t *>(buffer), width_, height_, x0, 0, x1 - x0,
    //     bar_h, bars[i][0], bars[i][1], bars[i][2]);
    // }
    overlay_clock_yuv422(static_cast<uint8_t *>(buffer), width_, height_, width_ * 2, fmt);
}

void V4L2Capturer::CaptureImage() {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd_, &fds);
    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200 ms
    int r = select(fd_ + 1, &fds, NULL, NULL, &tv);
    if (r == -1) {
        if (errno == EINTR) {
            return; // interrupted by a signal - not counted as a failure
        }
        ERROR_PRINT("select failed: %s", strerror(errno));
        HandleCaptureFailure("select error");
        return;
    } else if (r == 0) { // timeout
        DEBUG_PRINT("capture timeout");
        HandleCaptureFailure("capture timeout");
        return;
    }

    v4l2_buffer buf = {};
    buf.type = capture_.type;
    buf.memory = capture_.memory;

    if (!V4L2Util::DequeueBuffer(fd_, &buf)) {
        HandleCaptureFailure("dequeue buffer failed");
        return;
    }

    // Frame captured successfully - reset the consecutive failure counter.
    // (The device is alive even if this particular frame turns out corrupt.)
    capture_failure_count_ = 0;

    // Measure the REAL rate we dequeue frames from the camera (all frames, good or
    // corrupt). Logged every ~10s in debug. A drop well below the configured fps -
    // especially right after recording starts - means the capture loop can't keep up
    // and QBUF is delayed, starving the camera (USB isoc loss).
    {
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        if (capture_window_start_ms_ == 0) {
            capture_window_start_ms_ = now_ms;
        }
        capture_window_frames_++;
        int64_t el_ms = now_ms - capture_window_start_ms_;
        if (el_ms >= 10000) {
            DEBUG_PRINT("Capture rate /dev/video%d: %.1f fps dequeued "
                        "(%d frames / %.1fs, %d dropped bad) [configured %d]",
                        camera_id_, capture_window_frames_ * 1000.0 / (double)el_ms,
                        capture_window_frames_, el_ms / 1000.0, capture_window_dropped_, fps_);
            capture_window_frames_ = 0;
            capture_window_dropped_ = 0;
            capture_window_start_ms_ = now_ms;
        }
    }

    // Drop frames the kernel flagged as corrupt (lost USB isoc packets on the UVC
    // camera) or that are short for an uncompressed format. Otherwise the partial/
    // stale MMAP buffer gets the clock drawn on it and encoded -> the "рассыпание"
    // artifact. Dropping turns it into an honest fps drop instead of visible garbage.
    bool bad_frame = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
    if (!bad_frame && !IsCompressedFormat() &&
        buf.bytesused < (uint32_t)width_ * height_ * 2) {
        bad_frame = true;
    }
    if (bad_frame) {
        dropped_frame_count_++;
        capture_window_dropped_++;
        if (dropped_frame_count_ == 1 || dropped_frame_count_ % 30 == 0) {
            INFO_PRINT("Dropped corrupt/short camera frame "
                       "(flags=0x%x, bytesused=%u, total dropped=%d) — USB delivery loss",
                       buf.flags, buf.bytesused, dropped_frame_count_);
        }
        V4L2Util::QueueBuffer(fd_, &buf);
        return;
    }

    auto buffer = V4L2Buffer::FromV4L2((uint8_t *)capture_.buffers[buf.index].start, buf, format_);
    frame_buffer_ = V4L2FrameBuffer::Create(width_, height_, buffer);
    if (hw_accel_ && format_ == V4L2_PIX_FMT_H264) {
        if ((buffer.flags & V4L2_BUF_FLAG_KEYFRAME) != 0) {
            has_first_keyframe_ = true;
        }
        if (!has_first_keyframe_) {
            V4L2Util::QueueBuffer(fd_, &buf);
            return;
        }
    }
    if (draw_clock_ && (format_ == V4L2_PIX_FMT_YUYV || format_ == V4L2_PIX_FMT_UYVY)) {
        static bool first_frame = true;
        if (first_frame) {
            INFO_PRINT("Drawing debug info on %s frames", V4L2Util::FourccToString(format_).c_str());
            first_frame = false;
        }
        DrawDebugInfo((uint8_t *)capture_.buffers[buf.index].start);
    }
    if (hw_accel_ && IsCompressedFormat()) {
        if (!decoder_) {
            decoder_ = V4L2Decoder::Create({width_, height_, format_, true});
        }

        decoder_->EmplaceBuffer(frame_buffer_, [this, buffer](V4L2FrameBufferRef decoded_buffer) {
            // hw decoder doesn't output timestamps.
            decoded_buffer->GetRawBuffer().timestamp = buffer.timestamp;
            stream_subject_.Next(decoded_buffer);
        });
    } else {
        stream_subject_.Next(frame_buffer_);
    }

    if (!V4L2Util::QueueBuffer(fd_, &buf)) {
        return;
    }
}

void V4L2Capturer::HandleCaptureFailure(const char *reason) {
    // Tell a real disconnect from a transient stall: if QUERYCAP fails, the device
    // node is gone (USB unplug). Exit immediately so we release the fd and free the
    // /dev/videoN node — if we keep holding it (~5s of failures) while the camera
    // re-enumerates, it comes back as a new node (video0 -> video1) and a restart
    // that looks for the old node won't find it until a physical re-plug.
    v4l2_capability cap = {};
    if (!V4L2Util::QueryCapabilities(fd_, &cap)) {
        ERROR_PRINT("Camera disconnected (%s) - releasing the device and exiting now "
                    "so the service restarts and re-discovers it.",
                    reason);
        V4L2Util::CloseDevice(fd_);
        exit(EXIT_FAILURE);
    }
    if (++capture_failure_count_ >= kMaxCaptureFailures) {
        ERROR_PRINT("Camera produced no frames (%s) for too long - device likely "
                    "stuck. Exiting with error so the service restarts.",
                    reason);
        exit(EXIT_FAILURE);
    }
}

int V4L2Capturer::FindUsbCaptureDevice() {
    // Scan /dev/video* for the first USB video-capture node. Recovers from USB
    // re-enumeration that moved the camera off its configured number.
    for (int i = 0; i < 64; i++) {
        std::string path = "/dev/video" + std::to_string(i);
        int fd = V4L2Util::TryOpenDevice(path.c_str());
        if (fd < 0) {
            continue;
        }
        v4l2_capability cap = {};
        if (V4L2Util::QueryCapabilities(fd, &cap)) {
            uint32_t caps =
                (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
            bool is_capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
            bool is_usb = strncmp((const char *)cap.bus_info, "usb", 3) == 0;
            if (is_capture && is_usb) {
                INFO_PRINT("Found USB capture device at %s (card: %s, bus: %s)", path.c_str(),
                           cap.card, cap.bus_info);
                camera_id_ = i;
                return fd;
            }
        }
        V4L2Util::CloseDevice(fd);
    }
    return -1;
}

bool V4L2Capturer::SetControls(int key, int value) { return V4L2Util::SetExtCtrl(fd_, key, value); }

webrtc::scoped_refptr<webrtc::I420BufferInterface> V4L2Capturer::GetI420Frame(int stream_idx) {
    return frame_buffer_->ToI420();
}

Subscription V4L2Capturer::Subscribe(Subject<V4L2FrameBufferRef>::Callback callback,
                                     int stream_idx) {
    return stream_subject_.Subscribe(std::move(callback));
}

void V4L2Capturer::StartCapture() {
    if (!V4L2Util::AllocateBuffer(fd_, &capture_, buffer_count_) ||
        !V4L2Util::QueueBuffers(fd_, &capture_)) {
        // Should be rare now that Initialize() validates the frame size, but if
        // REQBUFS still fails exit non-zero so systemd restarts (and Create()'s
        // retry loop runs again) instead of exiting 0 and looking successful.
        ERROR_PRINT("Failed to allocate/queue capture buffers - exiting for restart");
        exit(EXIT_FAILURE);
    }

    V4L2Util::StreamOn(fd_, capture_.type);

    worker_ = std::make_unique<Worker>("V4L2 Capturer", [this]() {
        CaptureImage();
    });
    worker_->Run();
}
