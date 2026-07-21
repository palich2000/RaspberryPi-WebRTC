#ifndef ARGS_H_
#define ARGS_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <linux/videodev2.h>

enum RecordMode {
    Background = 0,
    OnDemand = 1,
};

enum RecordType {
    Video = 0,
    Snapshot = 1,
};

template <typename DEFAULT> struct TimeVal {
    TimeVal()
        : value(0) {}

    void set(const std::string &s) {
        static const std::unordered_map<std::string, std::chrono::nanoseconds> match{
            {"min", std::chrono::minutes(1)},     {"sec", std::chrono::seconds(1)},
            {"s", std::chrono::seconds(1)},       {"ms", std::chrono::milliseconds(1)},
            {"us", std::chrono::microseconds(1)}, {"ns", std::chrono::nanoseconds(1)},
        };

        try {
            std::size_t end_pos;
            float f = std::stof(s, &end_pos);
            value = std::chrono::duration_cast<std::chrono::nanoseconds>(f * DEFAULT{1});

            for (const auto &m : match) {
                auto found = s.find(m.first, end_pos);
                if (found != end_pos || found + m.first.length() != s.length())
                    continue;
                value = std::chrono::duration_cast<std::chrono::nanoseconds>(f * m.second);
                break;
            }
        } catch (std::exception const &e) {
            throw std::runtime_error("Invalid time string provided");
        }
    }

    template <typename C = DEFAULT> int64_t get() const {
        return std::chrono::duration_cast<C>(value).count();
    }

    explicit constexpr operator bool() const { return !!value.count(); }

    std::chrono::nanoseconds value;
};

struct Args {
    // video input
    int num_streams = 1;
    int camera_id = 0;
    int fps = 30;
    int width = 640;
    int height = 480;
    int rotation = 0;
    bool use_libargus = false;
    bool use_libcamera = false;
    uint32_t format = V4L2_PIX_FMT_MJPEG;
    std::string camera = "libcamera:0";
    std::string v4l2_format = "mjpeg";

    // Pin the UVC capture/DQBUF thread to this CPU core and move the camera's USB
    // host-controller IRQ onto the same core, isolating capture from I/O jitter on
    // the other cores (e.g. microSD writeback while recording, which otherwise
    // delays QBUF and drops whole frames). -1 (default) = disabled / unpinned.
    // Pair with kernel isolation (isolcpus + irqaffinity) on the same core for the
    // full effect. USB cameras only; ignored otherwise.
    int capture_cpu = -1;

    // how the encoder degrades under congestion/CPU limits:
    // framerate|balanced|resolution|disabled. Default framerate = the original
    // hardcoded MAINTAIN_FRAMERATE behavior (keep fps, drop resolution).
    std::string degradation = "framerate";

    // max encode bitrate in kbps applied to the video sender's RtpParameters;
    // 0 = leave the libwebrtc default (a resolution-based cap, ~1700 for VGA).
    // Raising it gives headroom that can lower QP and let the QualityScaler
    // upscale resolution back up after a downward excursion.
    int max_bitrate = 0;

    // sub stream for multiple resolution capture
    int sub_width = 0;
    int sub_height = 0;

    // stream usage, 0: main stream, 1: sub stream
    int record_stream_idx = 0; // recording stream index
    int live_stream_idx = 0;   // webrtc live stream index

    // audio input
    int sample_rate = 48000;
    int channels = 2;
    bool no_audio = false;
    bool force_alsa = false;

    // libcamera control options
    float sharpness = 1.0f;
    float contrast = 1.0f;
    float brightness = 0.0f;
    float saturation = 1.0f;
    float ev = 0.0f;
    std::string shutter_ = "0";
    TimeVal<std::chrono::microseconds> shutter;
    float gain = 0.0f;
    std::string ae_metering = "centre";
    int ae_metering_mode = 0;
    std::string exposure = "normal";
    int ae_mode = 0;
    std::string awb = "auto";
    int awb_mode = 0;
    std::string autofocus_mode = "default";
    int af_mode = -1;
    std::string awbgains = "0,0";
    float awb_gain_r = 0.0f;
    float awb_gain_b = 0.0f;
    std::string denoise = "auto";
    int denoise_mode = 0;
    std::string tuning_file = "-";
    std::string af_range = "normal";
    int af_range_mode = 0;
    std::string af_speed = "normal";
    int af_speed_mode = 0;
    std::string af_window = "0,0,0,0";
    float af_window_x = 0.0f;
    float af_window_y = 0.0f;
    float af_window_width = 0.0f;
    float af_window_height = 0.0f;
    std::string lens_position_ = "";
    std::optional<float> lens_position;
    bool set_default_lens_position = false;

    // recording
    std::string record_type_str = "both";
    int record_type = -1;
    std::string record_mode_str = "both";
    int record_mode = -1;
    std::string record_path = "";
    std::string record_ondemand_path = "";
    int file_duration = 60;
    int record_bitrate = 0; // recording video bitrate in kbps; 0 = auto (from resolution/fps)

    // ipc
    bool enable_ipc = false;
    std::string socket_path = "/tmp/pi-webrtc-ipc.sock";
    std::string ipc_channel = "both";
    int ipc_channel_mode = -1;

    // webrtc
    int jpeg_quality = 30;
    int peer_timeout = 10;
    bool hw_accel = false;
    bool no_adaptive = false;
    bool no_clock = false; // disable the clock overlay on the stream (clock is on by default)
    // OSD text overlay: a path glob (e.g. "/tmp/osd*"). Empty = disabled. When set,
    // the glob's directory is watched via inotify and matching files' first lines
    // are joined with " | " and drawn in the top-right corner of the stream.
    std::string osd = "";
    std::string uid = "";
    std::string stun_url = "stun:stun.l.google.com:19302";
    std::string turn_url = "";
    std::string turn_username = "";
    std::string turn_password = "";

    // mqtt signaling
    bool use_mqtt = false;
    int mqtt_port = 1883;
    std::string mqtt_host = "localhost";
    std::string mqtt_username = "";
    std::string mqtt_password = "";

    // http signaling
    bool use_whep = false;
    uint16_t http_port = 8080;

    // websocket signaling
    bool use_websocket = false;
    bool use_tls = false;
    std::string ws_host = "";
    uint16_t ws_port = 0; // 0 = default (443 with TLS, otherwise 80)
    std::string ws_room = "";
    std::string ws_key = "";
};

#endif // ARGS_H_
