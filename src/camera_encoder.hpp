#pragma once

#include <functional>
#include <memory>
#include <stop_token>
#include <string>

#include "common.hpp"
#include "vpacket.hpp"

namespace vacon {

struct CameraEncoderParams {
    std::string device;
    std::string codec;
    std::string camera_pixel_format;
    std::string encoder_pixel_format;
    int width;
    int height;
    int frame_rate;
    int bitrate;
};

class CameraEncoder {
    public:
        static std::unique_ptr<CameraEncoder> Create(const CameraEncoderParams& params);
        CameraEncoder(CameraEncoder&&) = default;
        ~CameraEncoder();

        bool encodePackets(std::stop_token st,
                           std::function<void(std::shared_ptr<VPacket>)> callback);
        bool processPacket(const AVPacket *packet);
        bool processFrame(const AVFrame *frame);
        bool encodeVideoFrame(const AVFrame *hw_frame);

    private:
        CameraEncoder() = default;
        std::function<void(std::shared_ptr<VPacket>)> callback;

        bool initCameraDevice();
        bool initCodecContext();
        bool initVaapiDevice();
        bool initVaapiHwFramePool();

        CameraEncoderParams params;

        // Video data being processed.
        AVFrame *frame, *hw_frame;
        AVPacket *pkt;

        // Things to do with the v4l2 input.
        int video_stream_idx;
        AVFormatContext *fmt_ctx;
        AVCodecContext *dec_ctx;

        // Things to do with the VAAPI encoder.
        AVBufferRef *hw_device_ctx;
        AVCodecContext *hw_ctx;
};

} // namespace vacon
