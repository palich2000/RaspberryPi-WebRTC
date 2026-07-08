#ifndef V4L2_H264_ENCODER_H_
#define V4L2_H264_ENCODER_H_

// WebRTC
#include <api/video_codecs/video_encoder.h>
#include <common_video/h264/h264_bitstream_parser.h>
#include <common_video/include/bitrate_adjuster.h>
#include <modules/video_coding/codecs/h264/include/h264.h>

#include "args.h"
#include "codecs/v4l2/v4l2_encoder.h"

class V4L2H264Encoder : public webrtc::VideoEncoder {
  public:
    static std::unique_ptr<webrtc::VideoEncoder> Create(Args args);
    V4L2H264Encoder(Args args);

    int32_t InitEncode(const webrtc::VideoCodec *codec_settings,
                       const VideoEncoder::Settings &settings) override;
    int32_t RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback) override;
    int32_t Release() override;
    int32_t Encode(const webrtc::VideoFrame &frame,
                   const std::vector<webrtc::VideoFrameType> *frame_types) override;
    void SetRates(const RateControlParameters &parameters) override;
    webrtc::VideoEncoder::EncoderInfo GetEncoderInfo() const override;

  protected:
    int width_;
    int height_;
    int fps_adjuster_;
    bool is_dma_;
    std::string name_;
    webrtc::VideoCodec codec_;
    webrtc::EncodedImage encoded_image_;
    webrtc::EncodedImageCallback *callback_;
    webrtc::BitrateAdjuster bitrate_adjuster_;
    std::unique_ptr<V4L2Encoder> encoder_;
    // Diagnostic logging state: emit one "encoder CC" line only when QP, target
    // bitrate, or fps shifts meaningfully (set in SetRates, logged in SendFrame).
    uint32_t cur_target_kbps_ = 0;
    int last_qp_ = -1;            // last parsed slice QP
    int last_logged_kbps_ = -1;   // target kbps at last emitted log
    int last_logged_qp_ = -100;   // QP at last emitted log
    int last_logged_fps_ = -1;    // fps at last emitted log
    webrtc::H264BitstreamParser h264_bitstream_parser_; // extracts per-frame QP for QualityScaler

    virtual void SendFrame(const webrtc::VideoFrame &frame, V4L2Buffer &encoded_buffer);
};

#endif
