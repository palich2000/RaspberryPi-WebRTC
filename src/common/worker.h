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
    Worker(std::string name, std::function<void()> executing_function,
           webrtc::ThreadPriority priority = webrtc::ThreadPriority::kHigh);
    ~Worker();
    void Run();

  private:
    std::atomic<bool> abort_;
    std::string name_;
    std::function<void()> executing_function_;
    webrtc::ThreadPriority priority_;
    webrtc::PlatformThread thread_;

    void Thread();
};

#endif // WORKER_H_
