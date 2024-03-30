// Copyright (c) 2024 The Vacon Authors
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

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <mfx.h>

#include "codecs.hpp"
#include "linux/camera.hpp"
#include "linux/typedefs.hpp"
#include "linux/video_frame.hpp"
#include "stats.hpp"

namespace vacon {
namespace linux {

extern std::atomic_size_t n_frames_encode_success;
extern std::atomic_size_t n_frames_encode_fail;
extern std::atomic_size_t n_frames_encode_stall;

struct EncoderParams {
    uint32_t bitrate_kbps;

    std::shared_ptr<CameraBufferQueue>
        encoder_queue = nullptr;

    std::shared_ptr<VideoPacketQueue>
        outgoing_video_packet_queue = nullptr;
};

class Encoder {
    public:
        static std::unique_ptr<Encoder> Create(const EncoderParams&);
        Encoder(Encoder&&) = default;
        ~Encoder();
        bool Init(const CameraFormat&);
        void RequestStop();
        void Join();
        std::vector<VideoCodec> GetSupportedCodecs();

        Welford             s_encode_size_ = {};
        Welford             s_encode_time_ = {};

    private:
        Encoder() = default;
        Encoder(const EncoderParams& params)
            : params_(params) {};
        void RunEncoder(std::stop_token);
        bool InitEncoder();
        bool InitMfxVideoParams();
        bool SetMfxFourCc();
        bool CopyCameraBufferToSurface(const CameraBufferRef&, mfxFrameSurface1&);
        std::shared_ptr<VideoFrame> EncodeCameraBuffer(const CameraBufferRef&);

        EncoderParams       params_ = {};
        CameraFormat        camera_format_ = {};

        std::jthread        thread_ = {};

        mfxSession          mfx_session_ = nullptr;
        mfxVideoParam       mfx_videoparam_encode_ = {};
        mfxVideoParam       mfx_videoparam_vpp_ = {};
        mfxExtCodingOption  mfx_eco1_ = {};
        mfxExtCodingOption2 mfx_eco2_ = {};
        mfxExtCodingOption3 mfx_eco3_ = {};
};

} // namespace linux
} // namespace vacon
