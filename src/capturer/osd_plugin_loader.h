//
// Loads PI_PLUGIN_KIND_OSD plugins (see plugin-api/pi_plugin_api.h) from .so
// files via dlopen() for V4L2Capturer. Each plugin draws into the raw captured
// frame on every capture and is driven live by JSON commands routed from the
// DataChannel (see Conductor::TryHandleOsdPluginCommand).
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One dlopen()'d OSD plugin. Owns the dlopen handle: calls pi_plugin_deinit()
// and dlclose() on destruction.
class LoadedOsdPlugin {
  public:
    // Loads and validates `path` as an OSD plugin (ABI version, kind, required
    // symbols, pi_plugin_init()). Returns nullptr on any failure - logged, never
    // fatal to the caller.
    static std::unique_ptr<LoadedOsdPlugin> Load(const std::string &path);

    ~LoadedOsdPlugin();
    LoadedOsdPlugin(const LoadedOsdPlugin &) = delete;
    LoadedOsdPlugin &operator=(const LoadedOsdPlugin &) = delete;

    const std::string &name() const { return name_; }

    // Draws into the raw frame buffer in place. Called on the capture thread,
    // once per frame; must be fast (see plugin-api/pi_plugin_api.h).
    //   yuv_fmt: 0 = YUYV, 1 = UYVY
    void Draw(uint8_t *img, int width, int height, int stride, int yuv_fmt);

    // Passes `request_json` through unmodified to the plugin; the plugin's
    // malloc'd response is freed here and returned as a std::string.
    std::string Command(const std::string &request_json);

  private:
    LoadedOsdPlugin() = default;

    void *handle_ = nullptr;
    std::string name_;
    // Raw C function pointers into the .so (typed in the .cpp against
    // plugin-api's typedefs) - kept as void* here to avoid pulling the plugin
    // ABI's C function-pointer types into this public header.
    void *draw_fn_ = nullptr;
    void *command_fn_ = nullptr;
    void *deinit_fn_ = nullptr;
};

// Loads every path in `paths` as an OSD plugin. A path that fails validation is
// logged and skipped (never fatal); the returned vector may be shorter than
// `paths`, or empty when `paths` is empty.
std::vector<std::unique_ptr<LoadedOsdPlugin>> LoadOsdPlugins(const std::vector<std::string> &paths);
