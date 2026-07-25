#ifndef VIDEO_CAPTURER_H_
#define VIDEO_CAPTURER_H_

#include <string>

#include "args.h"
#include "common/interface/subject.h"
#include "common/v4l2_frame_buffer.h"
#include "common/v4l2_utils.h"

// multiple video stream capturer interface
class VideoCapturer {
  public:
    VideoCapturer() = default;
    virtual ~VideoCapturer() = default;

    virtual int fps() const = 0;
    virtual int width(int stream_idx = 0) const = 0;
    virtual int height(int stream_idx = 0) const = 0;
    virtual bool has_sub_stream() const { return false; }
    virtual bool is_dma_capture() const = 0;
    virtual uint32_t format() const = 0;
    virtual Args config() const = 0;
    virtual void StartCapture() = 0;
    // Pause: stop streaming and fully release the capture device, keeping the
    // capturer object and its subscriptions alive (frames simply stop arriving).
    // Default no-op for capturers that do not support pausing.
    virtual void StopCapture() {}
    // Resume after StopCapture: reacquire the device and start streaming again.
    // Default just calls StartCapture(); capturers needing re-init (V4L2) override.
    virtual void ResumeCapture() { StartCapture(); }
    // Current V4L2 node this capturer is bound to, e.g. "/dev/video8". Reported so
    // pi-webrtc can stamp the device into forwarded IPC control requests and reach
    // the right node even after a runtime USB re-enumeration. Empty when unknown.
    virtual std::string device_path() const { return ""; }
    virtual webrtc::scoped_refptr<webrtc::I420BufferInterface> GetI420Frame(int stream_idx = 0) = 0;
    virtual bool SetControls(int key, int value) { return false; };
    virtual Subscription Subscribe(Subject<V4L2FrameBufferRef>::Callback callback,
                                   int stream_idx = 0) = 0;
};

#endif
