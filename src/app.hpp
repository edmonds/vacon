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

#include "linux/camera.hpp"
#include "linux/typedefs.hpp"
#include "linux/video_frame.hpp"
#include "linux/video_handler.hpp"
#include "packet_ref.hpp"
#include "network_handler.hpp"

namespace vacon {

class App {
    public:
        int AppInit(int argc, char *argv[]);
        int AppIterate();
        int AppEvent(const SDL_Event* event);
        void AppQuit();

        argparse::ArgumentParser                args_ =
            argparse::ArgumentParser(PROJECT_NAME, PROJECT_VERSION, argparse::default_arguments::none);

        int                                     verbosity_ = 0;

        SDL_Renderer*                           sdl_renderer_ = nullptr;
        SDL_Window*                             sdl_window_ = nullptr;

        std::unique_ptr<NetworkHandler>         nh_ = nullptr;
        std::unique_ptr<linux::VideoHandler>    vh_ = nullptr;
        std::shared_ptr<linux::CameraBufferRef> preview_cref_ = nullptr;

        std::shared_ptr<PacketRefQueue>         incoming_video_packet_queue_ =
            std::make_shared<PacketRefQueue>(2);

        std::shared_ptr<linux::VideoPacketQueue>    outgoing_video_packet_queue_ =
            std::make_shared<linux::VideoPacketQueue>(2);

    private:
        // app.cpp
        int ShutdownEvent();
        void ProcessUserEvent(const SDL_UserEvent*);
        void StartNetworkHandler();
        void StartVideoHandler();
        void StopNetworkHandler();
        void StopVideoHandler();

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
        void ShowPreview();
        void ShowPreviewWindow();
        void ProcessUiEvent(const SDL_Event*);

        float           font_size_sans_                 = 12.0f;
        float           font_size_mono_                 = 12.0f;
        bool            enable_my_camera_               = true;
        bool            enable_my_microphone_           = true;
        bool            enable_stats_overlay_           = true;
        bool            xenable_demo_window_            = false;
        bool            xenable_network_handler_        = false;
        bool            xenable_video_handler_          = false;

        bool            enable_self_view_               = true;
        bool            mirror_self_view_               = true;
        int             self_view_width_                = 512;
        int             self_view_height_               = 288;

        SDL_Texture*    sdl_texture_placeholder_        = nullptr;

        struct {
            unsigned    n_preview                       = 0;
            unsigned    n_preview_underflow             = 0;
        } stats_;
};

extern volatile std::sig_atomic_t gShuttingDown;
extern volatile std::sig_atomic_t gUSR1;

} // namespace vacon
