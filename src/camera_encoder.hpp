// Copyright (c) 2023-2024 The Vacon Authors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

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
