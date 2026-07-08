#include "codecs/v4l2/v4l2_h264_encoder.h"
#include "common/logging.h"
#include "common/v4l2_frame_buffer.h"

#include <api/array_view.h>
#include <modules/video_coding/include/video_codec_interface.h>
#include <modules/video_coding/include/video_error_codes.h>

std::unique_ptr<webrtc::VideoEncoder> V4L2H264Encoder::Create(Args args) {
    return std::make_unique<V4L2H264Encoder>(args);
}

V4L2H264Encoder::V4L2H264Encoder(Args args)
    : fps_adjuster_(args.fps),
      bitrate_adjuster_(.85, 1),
      callback_(nullptr) {}

int32_t V4L2H264Encoder::InitEncode(const webrtc::VideoCodec *codec_settings,
                                    const VideoEncoder::Settings &settings) {
    codec_ = *codec_settings;
    width_ = codec_settings->width;
    height_ = codec_settings->height;
    bitrate_adjuster_.SetTargetBitrateBps(codec_settings->startBitrate * 1000);

    encoded_image_.timing_.flags = webrtc::VideoSendTiming::TimingFrameFlags::kInvalid;
    encoded_image_.content_type_ = webrtc::VideoContentType::UNSPECIFIED;

    if (codec_.codecType != webrtc::kVideoCodecH264) {
        return WEBRTC_VIDEO_CODEC_ERROR;
    }

    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t V4L2H264Encoder::RegisterEncodeCompleteCallback(webrtc::EncodedImageCallback *callback) {
    callback_ = callback;
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t V4L2H264Encoder::Release() {
    encoder_.reset();
    return WEBRTC_VIDEO_CODEC_OK;
}

int32_t V4L2H264Encoder::Encode(const webrtc::VideoFrame &frame,
                                const std::vector<webrtc::VideoFrameType> *frame_types) {
    if (!frame_types) {
        return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }

    if ((*frame_types)[0] == webrtc::VideoFrameType::kEmptyFrame) {
        return WEBRTC_VIDEO_CODEC_OK;
    }
    webrtc::scoped_refptr<webrtc::VideoFrameBuffer> frame_buffer = frame.video_frame_buffer();

    if (frame_buffer->type() != webrtc::VideoFrameBuffer::Type::kNative) {
        return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }

    auto v4l2_frame_buffer = V4L2FrameBufferRef(static_cast<V4L2FrameBuffer *>(frame_buffer.get()));

    if (!encoder_) {
        EncoderConfig config;
        config.width = width_;
        config.height = height_;
        config.src_pix_fmt = V4L2_PIX_FMT_YUV420;
        config.is_dma_src = frame_buffer->type() == webrtc::VideoFrameBuffer::Type::kNative;
        config.keyframe_interval = 600;
        encoder_ = V4L2Encoder::Create(config);
    }

    if ((*frame_types)[0] == webrtc::VideoFrameType::kVideoFrameKey) {
        encoder_->ForceKeyFrame();
    }

    encoder_->EmplaceBuffer(v4l2_frame_buffer, [this, frame](V4L2FrameBufferRef encoded_buffer) {
        auto raw_buffer = encoded_buffer->GetRawBuffer();
        SendFrame(frame, raw_buffer);
    });

    return WEBRTC_VIDEO_CODEC_OK;
}

void V4L2H264Encoder::SetRates(const RateControlParameters &parameters) {
    if (parameters.bitrate.get_sum_bps() <= 0 || parameters.framerate_fps <= 0) {
        return;
    }
    bitrate_adjuster_.SetTargetBitrateBps(parameters.bitrate.get_sum_bps());
    fps_adjuster_ = parameters.framerate_fps;
    // Store for the consolidated diagnostic logged in SendFrame (where QP is known).
    cur_target_kbps_ = parameters.bitrate.get_sum_bps() / 1000;

    if (!encoder_) {
        return;
    }
    encoder_->SetFps(fps_adjuster_);
    encoder_->SetBitrate(bitrate_adjuster_.GetAdjustedBitrateBps());
}

webrtc::VideoEncoder::EncoderInfo V4L2H264Encoder::GetEncoderInfo() const {
    EncoderInfo info;
    info.supports_native_handle = true;
    info.is_hardware_accelerated = true;
    info.implementation_name = "Raspberry Pi V4L2 H264 Hardware Encoder";

    // Advertise QP thresholds so libwebrtc runs the QualityScaler for this HW
    // encoder. In MAINTAIN_FRAMERATE the QualityScaler is the ONLY thing that
    // scales resolution back UP: it drops resolution when the encoded QP stays
    // above the high threshold and raises it again when QP stays below the low one.
    // Without this (the previous default), the scaler was kOff and the stream got
    // stuck at a reduced resolution after a downward excursion. Requires SendFrame
    // to fill encoded_image_.qp_ from the bitstream.
    // low=33/high=42 (vs libwebrtc's H264 default 24/37): the upscale threshold is
    // raised so a busy scene -- which sits around QP 29-31 at a reduced resolution
    // even with plenty of bitrate -- clears the threshold and scales back up to full
    // resolution (where QP lands ~35-37, below high=42 so it stays put, no
    // oscillation), trading a slightly higher QP for the larger frame. Tune against
    // footage.
    info.scaling_settings = VideoEncoder::ScalingSettings(33, 42);
    // HW encoders default to "QP not trusted", which makes libwebrtc fall back to a
    // bandwidth-based scaler instead of the QP-based QualityScaler. We parse a real
    // slice QP in SendFrame, so mark it trusted to engage the QP scaler.
    info.is_qp_trusted = true;
    return info;
}

void V4L2H264Encoder::SendFrame(const webrtc::VideoFrame &frame, V4L2Buffer &encoded_buffer) {
    auto encoded_image_buffer =
        webrtc::EncodedImageBuffer::Create((uint8_t *)encoded_buffer.start, encoded_buffer.length);

    webrtc::CodecSpecificInfo codec_specific;
    codec_specific.codecType = webrtc::kVideoCodecH264;
    codec_specific.codecSpecific.H264.packetization_mode =
        webrtc::H264PacketizationMode::NonInterleaved;

    encoded_image_.SetEncodedData(encoded_image_buffer);
    encoded_image_.SetRtpTimestamp(frame.rtp_timestamp());
    encoded_image_.SetColorSpace(frame.color_space());
    encoded_image_._encodedWidth = width_;
    encoded_image_._encodedHeight = height_;
    encoded_image_.capture_time_ms_ = frame.render_time_ms();
    encoded_image_.ntp_time_ms_ = frame.ntp_time_ms();
    encoded_image_.rotation_ = frame.rotation();
    encoded_image_._frameType = encoded_buffer.flags & V4L2_BUF_FLAG_KEYFRAME
                                    ? webrtc::VideoFrameType::kVideoFrameKey
                                    : webrtc::VideoFrameType::kVideoFrameDelta;

    // Extract the slice QP from the Annex-B bitstream and hand it to libwebrtc so
    // the QualityScaler (enabled in GetEncoderInfo) can drive resolution up/down.
    h264_bitstream_parser_.ParseBitstream(webrtc::MakeArrayView(
        (const uint8_t *)encoded_buffer.start, encoded_buffer.length));
    if (auto qp = h264_bitstream_parser_.GetLastSliceQp()) {
        encoded_image_.qp_ = *qp;
        last_qp_ = *qp;
    }

    // Consolidated diagnostic: one line only when QP, target bitrate, or fps shifts
    // meaningfully -- shows scene-driven QP without per-frame spam.
    int dqp = last_qp_ - last_logged_qp_;
    if (dqp < 0) {
        dqp = -dqp;
    }
    int dkbps = (int)cur_target_kbps_ - last_logged_kbps_;
    if (dkbps < 0) {
        dkbps = -dkbps;
    }
    if (dqp >= 2 || dkbps >= 25 || fps_adjuster_ != last_logged_fps_) {
        last_logged_qp_ = last_qp_;
        last_logged_kbps_ = cur_target_kbps_;
        last_logged_fps_ = fps_adjuster_;
        INFO_PRINT("encoder CC: target=%u kbps, fps=%d, res=%dx%d, qp=%d", cur_target_kbps_,
                   fps_adjuster_, width_, height_, last_qp_);
    }

    auto result = callback_->OnEncodedImage(encoded_image_, &codec_specific);
    if (result.error != webrtc::EncodedImageCallback::Result::OK) {
        ERROR_PRINT("Failed to send the frame => %d", result.error);
    }
}
