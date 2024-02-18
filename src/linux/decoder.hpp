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

#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>

#include <SDL3/SDL.h>
#include <mfx.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <wayland-client.h>

#include "linux/camera.hpp"
#include "linux/typedefs.hpp"
#include "packet_ref.hpp"

namespace vacon {
namespace linux {

struct DecoderParams {
    std::shared_ptr<PacketRefQueue>     incoming_video_packet_queue = nullptr;
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
        bool Init();
        void RequestStop();
        void Join();

    private:
        Decoder() = default;
        Decoder(const DecoderParams& params)
            : params_(params) {};
        bool InitVaapi();
        void RunDecoder(std::stop_token);
        bool InitDecoder();
        void DecodePacket(std::shared_ptr<PacketRef>);

        DecoderParams       params_;

        std::jthread        thread_ = {};

        mfxLoader           mfx_loader_ = nullptr;
        mfxSession          mfx_session_ = nullptr;
        mfxVideoParam       mfx_videoparam_decode_ = {};
        bool                need_decode_init_ = true;

        VADisplay           va_display_ = {};
        wl_display*         wl_display_ = nullptr;

        uint32_t            n_frames_decoded_ = 0;
        uint32_t            n_frames_discarded_ = 0;
        uint32_t            n_frames_failed_ = 0;
};

} // namespace linux
} // namespace vacon
