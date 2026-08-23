#ifndef CONDUCTOR_H_
#define CONDUCTOR_H_

#include <condition_variable>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include <api/peer_connection_interface.h>
#include <rtc_base/thread.h>

#include "args.h"
#include "capturer/audio_capturer.h"
#include "capturer/video_capturer.h"
#include "recorder/recorder_manager.h"
#include "rtc/audio_device_bridge.h"
#include "rtc/rtc_peer.h"
#include "track/scale_track_source.h"

class Conductor {
  public:
    static std::shared_ptr<Conductor> Create(Args args);

    Conductor(Args args);
    ~Conductor();

    Args config() const;
    webrtc::scoped_refptr<RtcPeer> CreatePeerConnection(PeerConfig peer_config);
    std::shared_ptr<AudioCapturer> AudioSource() const;
    std::shared_ptr<VideoCapturer> VideoSource() const;
    void EnsureTracksAdded(webrtc::scoped_refptr<RtcPeer> peer);
    void SetOnDemandRecorder(std::shared_ptr<RecorderManager> recorder);

  private:
    Args args;

    void InitializePeerConnectionFactory();
    void InitializeTracks();
    void InitializeIpcServer();
    void InitializeDataChannels(webrtc::scoped_refptr<RtcPeer> peer);
    void InitializeCommandChannel(webrtc::scoped_refptr<RtcPeer> peer);
    void RegisterRecordingHandlers(std::shared_ptr<RtcChannel> channel);

    void BindIpcToDataChannel(std::shared_ptr<RtcChannel> channel);
    void BindIpcToDataChannelSender(std::shared_ptr<RtcChannel> channel);
    void BindDataChannelToIpcReceiver(std::shared_ptr<RtcChannel> channel);
    // Intercept a capture pause/resume command relayed by the SFU as CUSTOM JSON
    //   {"cmd":"capture","active":true|false}
    // before it is forwarded to the ipc_socket_client unix socket. Returns true
    // when the message was a capture command (and must NOT be forwarded), false
    // otherwise. Used for multi-camera single-active switching
    // (see TWO_CAMERA_SWITCH_PLAN.md).
    bool TryHandleCaptureCommand(const std::string &msg);
    // Route a CUSTOM JSON command carrying a "plugin" field to the matching
    // dlopen()'d OSD plugin on the active capturer (see
    // capturer/osd_plugin_loader.h). Returns true when the message carried a
    // "plugin" field at all (so it must NOT be forwarded elsewhere) - in that
    // case a JSON reply is ALWAYS sent back over `channel`, either the
    // plugin's own response or an explicit {"ok":false,...} when no plugin
    // with that name is loaded, so the caller never has to guess whether the
    // command took effect. Returns false when the message carries no "plugin"
    // field at all (not plugin-related, falls through to normal handling).
    bool TryHandleOsdPluginCommand(std::shared_ptr<RtcChannel> channel, const std::string &msg);
    // Stamp this instance's capture device into a forwarded control request so
    // ipc_socket_client (which may serve several cameras) targets the right V4L2
    // node. Returns the message with a "video_dev" field added; passes it through
    // unchanged when it is not a JSON object or already carries "video_dev".
    std::string InjectControlDevice(const std::string &msg);

    void AddTracks(webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_connection);
    void TakeSnapshot(std::shared_ptr<RtcChannel> datachannel, const protocol::Packet &pkt);
    void QueryFile(std::shared_ptr<RtcChannel> datachannel, const protocol::Packet &pkt);
    void TransferFile(std::shared_ptr<RtcChannel> datachannel, const protocol::Packet &pkt);
    void ControlCamera(std::shared_ptr<RtcChannel> datachannel, const protocol::Packet &pkt);
    void SendFileResponse(std::shared_ptr<RtcChannel> datachannel, const std::string &path,
                          const protocol::VideoMode mode);
    void StartRecording(std::shared_ptr<RtcChannel> datachannel, const protocol::Packet &pkt);
    void StopRecording(std::shared_ptr<RtcChannel> datachannel, const protocol::Packet &pkt);

    std::unique_ptr<webrtc::Thread> network_thread_;
    std::unique_ptr<webrtc::Thread> worker_thread_;
    std::unique_ptr<webrtc::Thread> signaling_thread_;

    bool use_alsa_audio_capture_ = false;
    std::shared_ptr<AudioCapturer> audio_capture_source_;
    std::shared_ptr<VideoCapturer> video_capture_source_;
    webrtc::scoped_refptr<AudioDeviceBridge> adm_;
    webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> peer_connection_factory_;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track_;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> video_track_;
    webrtc::scoped_refptr<ScaleTrackSource> video_track_source_;

    std::shared_ptr<UnixSocketServer> ipc_server_;
    std::weak_ptr<RecorderManager> ondemand_recorder_;
};

#endif // CONDUCTOR_H_
