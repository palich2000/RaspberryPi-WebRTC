#include "capturer/v4l2_capturer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

// Isolated pause/resume test for the V4L2 capturer (step 1 of the two-camera
// switching work, see TWO_CAMERA_SWITCH_PLAN.md). Runs a capture window, then
// repeatedly StopCapture()/ResumeCapture() and checks that:
//   - frames flow while capturing,
//   - NO frames arrive while paused (device fully released),
//   - frames flow again after each resume, with no hang or leak across cycles.
//
// Usage: ./test-v4l2-capturer [camera_id] [cycles]
//   camera_id defaults to 0, cycles defaults to 5.
// Adjust format/size below to the device under test (UYVY for MIPI, YUYV for the
// UVC thermal). Raw path, no hw encoder (matches Pi 5).
int main(int argc, char *argv[]) {
    int camera_id = (argc > 1) ? std::atoi(argv[1]) : 0;
    int cycles = (argc > 2) ? std::atoi(argv[2]) : 5;

    Args args{.camera_id = camera_id,
              .fps = 30,
              .width = 640,
              .height = 480,
              .format = V4L2_PIX_FMT_YUYV,
              .hw_accel = false};

    std::atomic<int> frame_count{0};
    auto capturer = V4L2Capturer::Create(args);
    // The subscription is kept alive across every pause/resume cycle - it must
    // keep delivering frames after a resume without re-subscribing.
    auto observer = capturer->Subscribe([&](V4L2FrameBufferRef frame_buffer) {
        frame_count.fetch_add(1, std::memory_order_relaxed);
    });

    for (int c = 0; c < cycles; c++) {
        int before = frame_count.load();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        int captured = frame_count.load() - before;
        printf("[cycle %d] capturing 2s -> %d frames\n", c, captured);

        printf("[cycle %d] StopCapture (release device)...\n", c);
        capturer->StopCapture();

        int paused_before = frame_count.load();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        int during_pause = frame_count.load() - paused_before;
        printf("[cycle %d] paused 1s -> %d frames (expected 0)\n", c, during_pause);

        printf("[cycle %d] ResumeCapture (reacquire device)...\n", c);
        capturer->ResumeCapture();
    }

    int before = frame_count.load();
    std::this_thread::sleep_for(std::chrono::seconds(2));
    printf("[final] after %d pause/resume cycles -> %d frames in 2s\n", cycles,
           frame_count.load() - before);
    printf("Done. Total frames: %d\n", frame_count.load());
    return 0;
}
