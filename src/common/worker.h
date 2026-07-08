#ifndef WORKER_H_
#define WORKER_H_

#include <atomic>
#include <functional>
#include <string>

#include <rtc_base/platform_thread.h>

class Worker {
  public:
    // priority defaults to kHigh (real-time) for latency-sensitive workers (capture,
    // cleaner). Pass kNormal for bulk work like recording: a real-time encoder thread
    // (and the openh264 child threads it spawns, which inherit its scheduling) preempts
    // the kernel's USB isoc handling and causes camera frame loss while recording.
    //
    // cpu_affinity (>= 0) pins this worker to a single CPU core. Used to isolate the
    // UVC capture/DQBUF thread onto a dedicated core (alongside the USB IRQ) so I/O
    // jitter from other cores (e.g. microSD writeback while recording) does not delay
    // QBUF and drop camera frames. -1 (default) leaves the thread unpinned.
    Worker(std::string name, std::function<void()> executing_function,
           webrtc::ThreadPriority priority = webrtc::ThreadPriority::kHigh,
           int cpu_affinity = -1);
    ~Worker();
    void Run();

  private:
    std::atomic<bool> abort_;
    std::string name_;
    std::function<void()> executing_function_;
    webrtc::ThreadPriority priority_;
    int cpu_affinity_;
    webrtc::PlatformThread thread_;

    void Thread();
};

#endif // WORKER_H_
