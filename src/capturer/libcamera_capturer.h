#ifndef LIBCAMERA_CAPTURER_H_
#define LIBCAMERA_CAPTURER_H_

#include <vector>

#include <libcamera/libcamera.h>
#include <modules/video_capture/video_capture.h>

#include "args.h"
#include "capturer/osd_overlay.h"
#include "capturer/osd_plugin_loader.h"
#include "capturer/video_capturer.h"
#include "common/interface/subject.h"
#include "common/v4l2_frame_buffer.h"
#include "common/v4l2_utils.h"
#include "common/worker.h"

class LibcameraCapturer : public VideoCapturer {
  public:
    static std::shared_ptr<LibcameraCapturer> Create(Args args);

    LibcameraCapturer(Args args);
    ~LibcameraCapturer() override;

    int fps() const override;
    int width(int stream_idx = 0) const override;
    int height(int stream_idx = 0) const override;
    bool is_dma_capture() const override;
    uint32_t format() const override;
    Args config() const override;

    bool SetControls(int key, int value) override;
    // Get/set a float-valued libcamera control (Brightness/Contrast today) by
    // its libcamera Control<float> descriptor, for the JSON get/set protocol
    // exposed over the IPC DataChannel (see
    // Conductor::TryHandleLibcameraControlCommand). Distinct from
    // SetControls(int,int) above, which is the legacy per-id protobuf path and
    // only handles integer-typed controls - Brightness/Contrast are float in
    // libcamera, so setting them via the int path would construct the wrong
    // ControlValue type. Returns false if this sensor/pipeline does not
    // advertise the control at all (queried from camera_->controls()).
    bool GetControlFloat(const libcamera::Control<float> &ctrl, float *value, float *min,
                         float *max, float *def);
    bool SetControlFloat(const libcamera::Control<float> &ctrl, float value);
    void StartCapture() override;
    // Stop streaming but keep the camera acquired/configured and the requests/
    // buffers allocated (mirrors V4L2Capturer's STREAMOFF-only pause) - RequestComplete
    // for the requests libcamera cancels during this is expected and swallowed (see
    // stopping_). No-op if not currently running.
    void StopCapture() override;
    // No-op if already running: the SFU sends {"cmd":"capture","active":true}
    // whenever a camera's IPC channel connects, not only on an actual
    // pause/resume, so the base class's blind StartCapture() would call
    // camera_->configure() on an already-Running camera and crash. If requests_
    // were already allocated (a real pause/resume, not first activation from
    // --start-passive), just reuse and re-queue them and restart the camera -
    // no configure()/AllocateBuffer() again.
    void ResumeCapture() override;

    webrtc::scoped_refptr<webrtc::I420BufferInterface> GetI420Frame(int stream_idx = 0) override;
    Subscription Subscribe(Subject<V4L2FrameBufferRef>::Callback callback,
                           int stream_idx = 0) override;

    // Same OSD-plugin command dispatch as V4L2Capturer::TryOsdPluginCommand.
    bool TryOsdPluginCommand(const std::string &plugin_name, const std::string &request_json,
                             std::string *response_json) override;

  private:
    int camera_id_;
    int fps_;
    int width_;
    int height_;
    int stride_;
    int rotation_;
    int buffer_count_;
    uint32_t format_;
    Args config_;
    std::mutex control_mutex_;
    std::atomic<bool> is_controls_updated_;
    std::atomic<bool> running_;
    // Set around an intentional camera_->stop() (StopCapture() or the destructor) so
    // RequestComplete() knows the resulting RequestCancelled callbacks are expected,
    // instead of treating cancellation as a fatal error.
    std::atomic<bool> stopping_;
    bool draw_clock_; // whether to draw the clock overlay on the stream (--no-clock disables it)
    std::unique_ptr<OsdOverlay> osd_;
    std::vector<std::unique_ptr<LoadedOsdPlugin>> osd_plugins_;

    std::unique_ptr<libcamera::CameraManager> cm_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> camera_config_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    libcamera::Stream *stream_;
    libcamera::ControlList controls_;
    std::map<int, std::pair<void *, unsigned int>> mapped_buffers_;

    V4L2FrameBufferRef frame_buffer_;
    Subject<V4L2FrameBufferRef> stream_subject_;

    void InitCamera();
    void InitControls(Args arg);
    void AllocateBuffer();
    void RequestComplete(libcamera::Request *request);
    void CameraDisconnected();
};

#endif
