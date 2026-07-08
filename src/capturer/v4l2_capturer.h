#ifndef V4L2_CAPTURER_H_
#define V4L2_CAPTURER_H_

#include <modules/video_capture/video_capture.h>

#include "args.h"
#include "capturer/video_capturer.h"
#include "codecs/v4l2/v4l2_decoder.h"
#include "common/interface/subject.h"
#include "common/v4l2_frame_buffer.h"
#include "common/v4l2_utils.h"
#include "common/worker.h"

class V4L2Capturer : public VideoCapturer {
  public:
    static std::shared_ptr<V4L2Capturer> Create(Args args);

    V4L2Capturer(Args args);
    ~V4L2Capturer() override;

    int fps() const override;
    int width(int stream_idx = 0) const override;
    int height(int stream_idx = 0) const override;
    bool is_dma_capture() const override;
    uint32_t format() const override;
    Args config() const override;

    bool SetControls(int key, int value) override;
    void StartCapture() override;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> GetI420Frame(int stream_idx = 0) override;
    Subscription Subscribe(Subject<V4L2FrameBufferRef>::Callback callback,
                           int stream_idx = 0) override;

  private:
    int camera_id_;
    int fd_;
    int fps_;
    int width_;
    int height_;
    int rotation_;
    int buffer_count_;
    bool hw_accel_;
    bool has_first_keyframe_;
    bool draw_clock_; // whether to draw the clock overlay on the stream (--no-clock disables it)
    uint32_t format_;
    Args config_;
    // Consecutive capture failures (timeout / select error / DQBUF error).
    // If the camera is unplugged, frames stop arriving - once the threshold
    // is exceeded we exit with an error so systemd restarts the service.
    int capture_failure_count_;
    // Total frames dropped because the kernel flagged them corrupt or short
    // (typically lost USB isoc packets on the UVC camera).
    int dropped_frame_count_;
    // Real DQBUF-rate measurement (debug). Counts frames pulled off the camera
    // per ~10s window and logs the actual capture fps. If this falls well below
    // the configured fps once recording starts, the capture loop is being starved
    // by downstream work (encode/mux) blocking QBUF -> USB frame loss.
    int capture_window_frames_ = 0;
    int capture_window_dropped_ = 0;
    int64_t capture_window_start_ms_ = 0;
    // select() waits 200ms, so ~25 consecutive failures = about 5s without frames.
    static constexpr int kMaxCaptureFailures = 25;
    // Patient re-open backoff (capped exponential) while waiting for a clean USB
    // enumeration. Replaces the tight ~5s systemd restart loop that hammered the
    // bridge mid-re-enumeration. See FindUsbCaptureDevice / Initialize.
    static constexpr int kInitBackoffMinSec = 2;
    static constexpr int kInitBackoffMaxSec = 30;
    V4L2BufferGroup capture_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<V4L2Decoder> decoder_;

    V4L2FrameBufferRef frame_buffer_;
    Subject<V4L2FrameBufferRef> stream_subject_;

    // Returns false on a RECOVERABLE failure (node absent, bad enumeration,
    // bogus negotiated frame size) so Create() can back off and retry. Only
    // genuine config errors (e.g. software H264) still exit().
    bool Initialize();
    void CloseFd();
    bool IsCompressedFormat() const;
    void CaptureImage();
    void HandleCaptureFailure(const char *reason);
    int FindUsbCaptureDevice();
    // Best-effort: if the active /dev/videoN is a USB camera, find its USB
    // host-controller IRQ and pin it to `cpu` (writes /proc/irq/<irq>/smp_affinity_list).
    // No-op + warning on any failure (not USB, no root, unexpected sysfs/proc layout).
    void PinUsbIrqToCpu(int cpu);
    void DrawDebugInfo(void *buffer);
    bool CheckMatchingDevice(std::string unique_name);
    int GetCameraIndex(webrtc::VideoCaptureModule::DeviceInfo *device_info);
};

#endif
