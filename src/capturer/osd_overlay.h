//
// OSD text overlay for packed YUV422 frames.
//
// Watches a directory (via inotify) for files whose basename matches a glob
// pattern, reads the first line of each matching file, joins the non-empty
// lines with " | " and draws the result in the top-right corner of every
// frame. The composed text is recomputed only when the watched files change
// (create / close-write / delete / move), so the per-frame Draw() call just
// renders the cached string.
//
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <stdint.h>

#include "yuyv_clock.h" // yuv422_fmt_t

class OsdOverlay {
  public:
    // `pattern` is a path glob, e.g. "/tmp/osd*". Its directory part is watched
    // via inotify; its basename part is used as an fnmatch() pattern.
    explicit OsdOverlay(const std::string &pattern);
    ~OsdOverlay();

    OsdOverlay(const OsdOverlay &) = delete;
    OsdOverlay &operator=(const OsdOverlay &) = delete;

    // Draw the cached text (if any) in the top-right corner of a packed YUV422
    // frame. Cheap: copies the cached string under a short lock, then renders.
    void Draw(uint8_t *img, int width, int height, int stride, yuv422_fmt_t fmt);

  private:
    void WatchLoop();
    void Rescan();

    std::string dir_;  // directory to watch
    std::string glob_; // basename fnmatch pattern

    std::atomic<bool> running_;
    int stop_pipe_[2]; // self-pipe to unblock the watcher on shutdown
    std::thread thread_;

    std::mutex mtx_;
    std::string text_; // composed OSD line, guarded by mtx_
};
