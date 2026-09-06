#include "args.h"
#include "capturer/libcamera_capturer.h"

#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

void WriteImage(void *start, int length, int index) {
    printf("Dequeued buffer index: %d\n"
           "  bytesused: %d\n",
           index, length);

    std::string filename = "img" + std::to_string(index) + ".yuv";
    int outfd = open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if ((outfd == -1) && (EEXIST == errno)) {
        /* open the existing file with write flag */
        outfd = open(filename.c_str(), O_WRONLY);
    }

    write(outfd, start, length);
}

// Captures a few frames, then StopCapture()/ResumeCapture() for several cycles,
// to validate the pause/resume path in isolation before it is wired into the
// SFU capture command (LIBCAMERA_INTEGRATION_PLAN.md Phase 2, "isolated pause/
// resume stress test"). StopCapture()/ResumeCapture() are called from this
// (main) thread, never from inside the Subscribe() callback - the callback runs
// on libcamera's own request-completion thread, and camera_->stop() blocks
// until that thread has drained all in-flight requests, so calling it from
// there would deadlock.
int main(int argc, char *argv[]) {
    std::mutex mtx;
    std::condition_variable cond_var;
    const int frames_per_cycle = 5;
    const int cycles = 5;
    int frames_in_cycle = 0;
    bool cycle_done = false;
    int cycle = 0;
    Args args{.fps = 30, .width = 1280, .height = 960};

    auto capturer = LibcameraCapturer::Create(args);

    auto observer = capturer->Subscribe([&](V4L2FrameBufferRef frame_buffer) {
        std::lock_guard<std::mutex> lock(mtx);
        if (frames_in_cycle < frames_per_cycle) {
            auto buffer = frame_buffer->GetRawBuffer();
            WriteImage(buffer.start, buffer.length, cycle * frames_per_cycle + ++frames_in_cycle);
            if (frames_in_cycle == frames_per_cycle) {
                cycle_done = true;
                cond_var.notify_all();
            }
        }
    });

    for (cycle = 0; cycle < cycles; cycle++) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            frames_in_cycle = 0;
            cycle_done = false;
            cond_var.wait(lock, [&] {
                return cycle_done;
            });
        }
        printf("=== Cycle %d: got %d frames, calling StopCapture() ===\n", cycle, frames_per_cycle);
        capturer->StopCapture();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        printf("=== Cycle %d: calling ResumeCapture() ===\n", cycle);
        capturer->ResumeCapture();
    }

    printf("Pause/resume stress test finished cleanly (%d cycles, no crash).\n", cycles);
    return 0;
}
