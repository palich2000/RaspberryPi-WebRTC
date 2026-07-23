//
// OSD text overlay for packed YUV422 frames.
//
// Watches a directory (via inotify) for files whose basename matches a glob
// pattern, reads the first line of each matching file, joins the non-empty
// lines with " | " and draws the result over every frame. The composed text
// AND the render style are recomputed only when the watched files change
// (create / close-write / delete / move), so the per-frame Draw() call just
// renders the cached string with the cached style - no per-frame parsing, so
// no added latency on the hot path.
//
// Placement / look are read from an optional style file in the same watched
// directory whose basename matches "*-position.txt" (kPositionGlob). That file
// is parsed as `key value` lines and is itself excluded from the OSD text.
// When absent, the default style reproduces the old behaviour exactly
// (top-right, opaque box, scale 2, zero offset).
//
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <stdint.h>

#include "yuyv_clock.h" // yuv422_fmt_t

// Vertical / horizontal anchor for the OSD box.
enum class OsdVAlign {
    Top,
    Bottom
};
enum class OsdHAlign {
    Left,
    Right
};

// Background / readability treatment behind the text.
//   Box     - opaque black box (default, legacy look)
//   BoxSemi - semi-transparent (blended) black box
//   None    - no background, plain text
//   Shadow  - no background, dark drop shadow under the text
enum class OsdBgMode {
    Box,
    BoxSemi,
    None,
    Shadow
};

// Render style, refreshed off the hot path by the watcher thread.
struct OsdStyle {
    OsdVAlign v_align = OsdVAlign::Top;
    OsdHAlign h_align = OsdHAlign::Right;
    int v_offset = 0; // pixels from the aligned vertical edge
    int h_offset = 0; // pixels from the aligned horizontal edge
    OsdBgMode bg = OsdBgMode::Box;
    int scale = 2; // integer font scale, >= 1
};

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
    OsdStyle style_;   // render style, guarded by mtx_

    // Set once we have failed to create the default style file, to avoid
    // logging the same error on every rescan. Touched only in the watcher
    // thread (Rescan), so it needs no lock.
    bool create_warned_ = false;
};
