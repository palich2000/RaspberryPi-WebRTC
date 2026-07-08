#include "common/worker.h"
#include "common/logging.h"

#include <cerrno>
#include <cstring>
#include <sched.h>

Worker::Worker(std::string name, std::function<void()> executing_function,
               webrtc::ThreadPriority priority, int cpu_affinity)
    : abort_(false),
      name_(name),
      executing_function_(executing_function),
      priority_(priority),
      cpu_affinity_(cpu_affinity) {}

Worker::~Worker() {
    abort_.store(true);
    thread_.Finalize();
    DEBUG_PRINT("'%s' was released!", name_.c_str());
}

void Worker::Run() {
    thread_ = webrtc::PlatformThread::SpawnJoinable(
        [this]() {
            this->Thread();
        },
        name_, webrtc::ThreadAttributes().SetPriority(priority_));
}

void Worker::Thread() {
    // webrtc's kNormal still maps to SCHED_FIFO (real-time) on Linux, so it would
    // preempt the kernel's normal-priority USB/uvcvideo softirq work and make the
    // camera drop frames. For a kNormal worker, force a TRUE SCHED_OTHER (nice 0) on
    // this thread BEFORE the executing function runs - so any threads it spawns
    // (e.g. openh264's encoder pool) inherit SCHED_OTHER too and compete fairly with
    // ksoftirqd instead of preempting it.
    if (priority_ == webrtc::ThreadPriority::kNormal) {
        struct sched_param sp = {};
        sp.sched_priority = 0;
        if (sched_setscheduler(0, SCHED_OTHER, &sp) != 0) {
            DEBUG_PRINT("'%s': sched_setscheduler(SCHED_OTHER) failed", name_.c_str());
        }
    }

    // Pin this worker to a single core (e.g. the isolated capture core) so it runs
    // next to the USB IRQ and is shielded from jitter on the other cores. Applied to
    // the worker thread itself (pid 0); any threads it later spawns inherit this mask.
    if (cpu_affinity_ >= 0) {
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(cpu_affinity_, &cpus);
        if (sched_setaffinity(0, sizeof(cpus), &cpus) != 0) {
            ERROR_PRINT("'%s': sched_setaffinity(CPU%d) failed: %s", name_.c_str(), cpu_affinity_,
                        strerror(errno));
        } else {
            INFO_PRINT("'%s' pinned to CPU%d", name_.c_str(), cpu_affinity_);
        }
    }

    while (!abort_.load()) {
        executing_function_();
    }
}
