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

#include <csignal>
#include <memory>

#include <SDL3/SDL.h>
#include <argparse/argparse.hpp>

#include "invite.hpp"
#include "linux/camera.hpp"
#include "linux/decoder.hpp"
#include "linux/encoder.hpp"
#include "linux/typedefs.hpp"
#include "linux/video_frame.hpp"
#include "network_handler.hpp"
#include "stats.hpp"

namespace vacon {

static const std::string kAppDefaultSignalingServer = "public.vacon.vc:30307";

class App {
    public:
        int AppInit(int argc, char *argv[]);
        int AppIterate();
        int AppEvent(const SDL_Event* event);
        void AppQuit();

    private:
        // app.cpp
        int ShutdownEvent();
        void ProcessUserEvent(const SDL_UserEvent*);
        void StartNetworkHandler();
        void StartVideo();
        void StartVideoCamera();
        void StartVideoDecoder();
        void StartVideoEncoder();
        void StopNetworkHandler();
        void StopVideo();

        void CreateConference();
        void CopyInviteToClipboard();
        void JoinConferenceFromClipboard();

        // args.cpp
        void ParseArgs(int argc, char *argv[]);

        // sdl.cpp
        bool InitSDL();
        bool InitSDLRenderer();
        bool InitSDLTextures();

        // ui.cpp
        bool InitImgui();
        void CalculateUiSize();
        void ShowMenu();
        void ShowStatsOverlay(bool*);
        void RenderFrame();
        void ShowDecodedVideoFrame();
        void ShowPreview();
        void ShowPreviewWindow();
        void ProcessUiEvent(const SDL_Event*);

        argparse::ArgumentParser
            args_ =
                argparse::ArgumentParser(PROJECT_NAME,
                                         PROJECT_VERSION,
                                         argparse::default_arguments::none);

        int             verbosity_                      = 0;

        int             n_camera_timeouts_              = 0;

        float           font_size_sans_                 = 14.0f;
        float           font_size_mono_                 = 10.0f;
        bool            enable_my_camera_               = true;
        bool            enable_my_microphone_           = true;
        bool            enable_stats_overlay_           = true;
        bool            xxx_enable_imgui_demo_window_   = false;

        bool            enable_self_view_               = true;
        bool            mirror_self_view_               = true;
        int             self_view_width_                = 512;
        int             self_view_height_               = 288;

        SDL_Renderer*   sdl_renderer_                   = nullptr;
        SDL_Texture*    sdl_texture_placeholder_        = nullptr;
        SDL_Window*     sdl_window_                     = nullptr;

        Welford         s_display_time_                 = {};
        Welford         s_present_time_                 = {};
        Welford         s_render_time_                  = {};

        std::unique_ptr<linux::Camera>
            camera_                                     = nullptr;

        std::unique_ptr<linux::Decoder>
            decoder_                                    = nullptr;

        std::unique_ptr<linux::Encoder>
            encoder_                                    = nullptr;

        std::unique_ptr<NetworkHandler>
            nh_                                         = nullptr;

        std::shared_ptr<linux::CameraBufferRef>
            preview_cref_                               = nullptr;

        std::shared_ptr<linux::DecodedFrame>
            decoded_frame_                              = nullptr;

        std::shared_ptr<linux::CameraBufferQueue>
            encoder_queue_                              = std::make_shared<linux::CameraBufferQueue>(2);

        std::shared_ptr<linux::CameraBufferQueue>
            preview_queue_                              = std::make_shared<linux::CameraBufferQueue>(2);

        std::shared_ptr<linux::DecodedFrameQueue>
            decoded_video_frame_queue_                  = std::make_shared<linux::DecodedFrameQueue>(4);

        std::shared_ptr<RtcPacketQueue>
            incoming_video_packet_queue_                = std::make_shared<RtcPacketQueue>(2);

        std::shared_ptr<linux::VideoPacketQueue>
            outgoing_video_packet_queue_                = std::make_shared<linux::VideoPacketQueue>(2);

        struct {
            unsigned    n_remote                        = 0;
            unsigned    n_remote_underflow              = 0;
            unsigned    n_preview                       = 0;
            unsigned    n_preview_underflow             = 0;
        } stats_;

        std::shared_ptr<Invite>
            invite_                                     = nullptr;
};

extern volatile std::sig_atomic_t gShuttingDown;
extern volatile std::sig_atomic_t gUSR1;

} // namespace vacon
