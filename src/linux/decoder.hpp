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
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <mfx.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <wayland-client.h>

#include "codecs.hpp"
#include "linux/typedefs.hpp"
#include "rtc_packet.hpp"
#include "stats.hpp"

namespace vacon {
namespace linux {

extern std::atomic_size_t n_frames_decode_success;
extern std::atomic_size_t n_frames_decode_fail;
extern std::atomic_size_t n_frames_decode_overflow;

struct DecoderParams {
    std::shared_ptr<RtcPacketQueue>     incoming_video_packet_queue = nullptr;
    std::shared_ptr<DecodedFrameQueue>  decoded_video_frame_queue = nullptr;
};

class DecodedFrame {
    public:
        DecodedFrame() = default;
        DecodedFrame(DecodedFrame&&);
        ~DecodedFrame();
        bool ExportToOpenGL(SDL_Renderer*);

        mfxFrameSurface1*               surface_ = nullptr;
        mfxSurfaceVAAPI*                exported_surface_ = nullptr;
        VADRMPRIMESurfaceDescriptor     prime_ = {};
        SDL_Texture*                    texture_ = nullptr;
};

class Decoder {
    public:
        static std::unique_ptr<Decoder> Create(const DecoderParams&);
        Decoder(Decoder&&) = default;
        ~Decoder();
        void StartThread(VideoCodec);
        void RequestStop();
        void Join();

        std::shared_ptr<std::vector<VideoCodec>> GetSupportedCodecs();

        VideoCodec Codec() const { return codec_; }

        Welford             s_decode_time_ = {};

    private:
        Decoder() = default;
        Decoder(const DecoderParams& params)
            : params_(params) {};
        bool InitVaapi();
        void RunDecoder(std::stop_token);
        void DecodePacket(std::shared_ptr<RtcPacket>);

        DecoderParams       params_;
        VideoCodec          codec_ = VideoCodec::UNKNOWN;

        std::jthread        thread_ = {};

        mfxSession          mfx_session_ = nullptr;
        mfxVideoParam       mfx_videoparam_decode_ = {};
        bool                need_decode_init_ = true;

        VADisplay           va_display_ = {};
        wl_display*         wl_display_ = nullptr;
};

} // namespace linux
} // namespace vacon
