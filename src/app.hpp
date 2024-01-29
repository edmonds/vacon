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

//#include <csignal>
//#include <cstdint>
//#include <memory>
//#include <thread>
//#include <vector>

#include <SDL3/SDL.h>
#include <argparse/argparse.hpp>
//#include <readerwritercircularbuffer.h>

//#include "linux/video_handler.hpp"
//#include "network_handler.hpp"

namespace vacon {

/*
typedef moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<linux::VideoFrame>>
    VideoPacketQueue;
*/

class App {
    public:
        int AppInit(int argc, char *argv[]);
        int AppIterate();
        int AppEvent(const SDL_Event *event);
        void AppQuit();

        /*
        static void SignalTerminate(int signal = 0);

        bool Setup(int argc, char *argv[]);
        void Shutdown();

        void StartNetworkHandler();
        void StartVideoHandler();

        void StartNetworkHandlerBackground();
        void StartVideoHandlerBackground();

        void StopNetworkHandler();
        void StopVideoHandler();
        */

        argparse::ArgumentParser                args_ = argparse::ArgumentParser(PROJECT_NAME,
                                                                                 PROJECT_VERSION,
                                                                                 argparse::default_arguments::none);
        int                                     verbosity_ = 0;

        SDL_RendererFlip                        sdl_renderer_flip_ = SDL_FLIP_NONE;
        SDL_Renderer*                           sdl_renderer_ = nullptr;
        SDL_Window*                             sdl_window_ = nullptr;

        /*
        std::shared_ptr<VideoPacketQueue>       outgoing_video_packet_queue_ = nullptr;
        std::unique_ptr<NetworkHandler>         nh_ = nullptr;
        std::unique_ptr<linux::VideoHandler>    vh_ = nullptr;
        std::vector<std::jthread>               threads_ = {};
        */

    private:
        // args.cpp
        void ParseArgs(int argc, char *argv[]);

        // sdl.cpp
        bool InitSDL();
        bool InitSDLRenderer();

        // ui.cpp
        bool InitImgui();
        void CalculateUiSize();
        void ShowMenu();
        void ShowStatsOverlay(bool*);
        void RenderFrame();
        void ProcessUiEvent(const SDL_Event*);

        float   font_size_sans_                 = 12.0f;
        float   font_size_mono_                 = 12.0f;
        bool    enable_my_camera_               = true;
        bool    enable_my_microphone_           = true;
        bool    enable_self_view_               = true;
        bool    enable_stats_overlay_           = true;
        bool    xenable_demo_window_            = false;
        bool    xenable_network_handler_        = false;
        bool    xenable_video_handler_          = false;

        struct {
            unsigned    n_preview               = 0;
            unsigned    n_preview_underflow     = 0;
        } stats_;
};

} // namespace vacon