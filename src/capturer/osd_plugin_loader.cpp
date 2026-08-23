#include "osd_plugin_loader.h"

#include <dlfcn.h>
#include <cstdlib>
#include <cstring>

#include "common/logging.h"
#include "pi_plugin_api.h"

namespace {

using InfoFn = const pi_plugin_info_t *(*)(void);
using InitFn = int (*)(const char *);
using CommandFn = char *(*)(const char *);
using DrawFn = void (*)(uint8_t *, int, int, int, int);
using DeinitFn = void (*)(void);

} // namespace

std::unique_ptr<LoadedOsdPlugin> LoadedOsdPlugin::Load(const std::string &path) {
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        ERROR_PRINT("OSD plugin: dlopen('%s') failed: %s", path.c_str(), dlerror());
        return nullptr;
    }

    dlerror(); // clear any pending error before dlsym(), per dlsym(3)
    auto info_fn = reinterpret_cast<InfoFn>(dlsym(handle, "pi_plugin_info"));
    auto init_fn = reinterpret_cast<InitFn>(dlsym(handle, "pi_plugin_init"));
    auto command_fn = reinterpret_cast<CommandFn>(dlsym(handle, "pi_plugin_command"));
    auto draw_fn = reinterpret_cast<DrawFn>(dlsym(handle, "pi_plugin_draw"));
    auto deinit_fn = reinterpret_cast<DeinitFn>(dlsym(handle, "pi_plugin_deinit"));
    if (!info_fn || !init_fn || !command_fn || !draw_fn || !deinit_fn) {
        ERROR_PRINT("OSD plugin: '%s' is missing a required symbol", path.c_str());
        dlclose(handle);
        return nullptr;
    }

    const pi_plugin_info_t *info = info_fn();
    if (!info || info->abi_version != PI_PLUGIN_ABI_VERSION) {
        ERROR_PRINT("OSD plugin: '%s' ABI version mismatch (got %d, want %d)", path.c_str(),
                    info ? info->abi_version : -1, PI_PLUGIN_ABI_VERSION);
        dlclose(handle);
        return nullptr;
    }
    if (info->kind != PI_PLUGIN_KIND_OSD) {
        ERROR_PRINT("OSD plugin: '%s' is not an OSD plugin (kind=%d)", path.c_str(), info->kind);
        dlclose(handle);
        return nullptr;
    }
    if (!info->name || info->name[0] == '\0') {
        ERROR_PRINT("OSD plugin: '%s' declares an empty name", path.c_str());
        dlclose(handle);
        return nullptr;
    }

    if (init_fn("{}") != 0) {
        ERROR_PRINT("OSD plugin: '%s' (%s) pi_plugin_init() failed", path.c_str(), info->name);
        dlclose(handle);
        return nullptr;
    }

    std::unique_ptr<LoadedOsdPlugin> plugin(new LoadedOsdPlugin());
    plugin->handle_ = handle;
    plugin->name_ = info->name;
    plugin->draw_fn_ = reinterpret_cast<void *>(draw_fn);
    plugin->command_fn_ = reinterpret_cast<void *>(command_fn);
    plugin->deinit_fn_ = reinterpret_cast<void *>(deinit_fn);

    INFO_PRINT("OSD plugin: loaded '%s' as plugin '%s'", path.c_str(), plugin->name_.c_str());
    return plugin;
}

LoadedOsdPlugin::~LoadedOsdPlugin() {
    if (!handle_) {
        return;
    }
    reinterpret_cast<DeinitFn>(deinit_fn_)();
    dlclose(handle_);
}

void LoadedOsdPlugin::Draw(uint8_t *img, int width, int height, int stride, int yuv_fmt) {
    reinterpret_cast<DrawFn>(draw_fn_)(img, width, height, stride, yuv_fmt);
}

std::string LoadedOsdPlugin::Command(const std::string &request_json) {
    char *resp = reinterpret_cast<CommandFn>(command_fn_)(request_json.c_str());
    if (!resp) {
        return {};
    }
    std::string result(resp);
    free(resp);
    return result;
}

std::vector<std::unique_ptr<LoadedOsdPlugin>> LoadOsdPlugins(const std::vector<std::string> &paths) {
    std::vector<std::unique_ptr<LoadedOsdPlugin>> plugins;
    plugins.reserve(paths.size());
    for (const auto &path : paths) {
        auto plugin = LoadedOsdPlugin::Load(path);
        if (plugin) {
            plugins.push_back(std::move(plugin));
        }
    }
    return plugins;
}
