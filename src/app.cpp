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

#include "app.hpp"

#include <SDL3/SDL.h>
#include <plog/Log.h>

#include "event.hpp"
#include "linux/video_handler.hpp"
#include "network_handler.hpp"
#include "util.hpp"

namespace vacon {

int App::AppInit(int argc, char *argv[])
{
    ParseArgs(argc, argv);

    util::SetupLogging(verbosity_);

    //SetupSignals(args["--usr1"] == true);

    if (!util::SetupRealtimePriority()) {
        LOG_ERROR << "Unable to set real-time thread priority, performance may be affected!";
    }

    if (!InitSDL()) {
        LOG_FATAL << "App::InitSDL() failed";
        return -1;
    }

    if (!InitImgui()) {
        LOG_FATAL << "App::InitImgui() failed";
        return -1;
    }

#if 0
    if (args["--xxx-force-start-network-handler"] == true) {
        StartNetworkHandlerBackground();
    }

    if (args["--xxx-force-start-video-handler"] == true) {
        StartVideoHandlerBackground();
    }
#endif

    return 0;
}

void App::AppQuit()
{
    // Drain any packets remaining on the outgoing video packet queue.
    if (outgoing_video_packet_queue_) {
        std::shared_ptr<linux::VideoFrame> frame = {};
        while (outgoing_video_packet_queue_->try_dequeue(frame)) {
            LOG_VERBOSE << std::format("Drained VideoFrame @ {}", (void*)frame.get());
        }
    }
    outgoing_video_packet_queue_ = nullptr;

    // Stop the handlers.
    StopVideoHandler();
    StopNetworkHandler();

#if 0
    // Join the handler threads.
    PLOG_INFO << "Waiting for threads to exit...";
    for (auto& thread : threads) {
        if (thread.joinable()) {
            PLOG_DEBUG << "Trying to join thread ID " << thread.get_id();
            thread.join();
        } else {
            PLOG_FATAL << "Thread ID " << thread.get_id() << " is not joinable ?!";
        }
    }
#endif
}

int App::AppEvent(const SDL_Event *event)
{
    ProcessUiEvent(event);

    if (event->type == SDL_EVENT_QUIT) {
        return 1;
    }

    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        event->window.windowID == SDL_GetWindowID(sdl_window_))
    {
        return 1;
    }

    if (event->type == SDL_EVENT_USER) {
        ProcessUserEvent(&event->user);
    }

    return 0;
}

void App::ProcessUserEvent(const SDL_UserEvent *user)
{
    auto event = static_cast<Event>(user->code);

    switch (event) {

    case Event::CameraStarting:
        break;

    case Event::CameraStarted:
        if (vh_ && sdl_renderer_) {
            LOG_DEBUG << "Calling Camera::ExportBuffersToOpenGL() on render thread";
            vh_->camera_->ExportBuffersToOpenGL(sdl_renderer_);
        }
        break;

    case Event::CameraFailed:
        break;

    case Event::NetworkStarting:
        break;

    case Event::NetworkStarted:
        break;

    case Event::NetworkFailed:
        break;

    default:
        LOG_DEBUG << "Unknown event code " << user->code;
    }
}

int App::AppIterate()
{
    RenderFrame();

    return 0;
}

void App::StartVideoHandler()
{
    if (vh_) {
        return;
    }

    // Get camera parameters.
    std::optional<linux::CameraParams> camera_params = {
        linux::CameraParams {
            .device         = args_.get<std::string>("--camera-device"),
            .pixel_format   = args_.get<std::string>("--camera-pixel-format"),
            .width          = args_.get<unsigned>("--camera-width"),
            .height         = args_.get<unsigned>("--camera-height"),
            .frame_rate     = args_.get<unsigned>("--camera-frame-rate"),
        }
    };

    // Get video encoder parameters.
    std::optional<linux::EncoderParams> encoder_params = {
        linux::EncoderParams {
            .pixel_format   = args_.get<std::string>("--camera-pixel-format"),
            .width          = args_.get<unsigned>("--camera-width"),
            .height         = args_.get<unsigned>("--camera-height"),
            .frame_rate     = args_.get<unsigned>("--camera-frame-rate"),
            .bitrate_kbps   = args_.get<unsigned>("--video-encoder-bitrate"),
        }
    };

    auto params = linux::VideoHandlerParams {
        .camera_params = camera_params,
        .encoder_params = encoder_params,
        .outgoing_video_packet_queue = outgoing_video_packet_queue_,
    };

    vh_ = linux::VideoHandler::Create(params);
    if (!vh_) {
        LOG_FATAL << "VideoHandler::Create() failed!";
        return;
    }

    vh_->Init();
}

void App::StopVideoHandler()
{
    if (vh_) {
        // All outstanding CameraBufferRef's need to be destroyed before the
        // VideoHandler (and its Camera) can be destroyed, because destroying a
        // CameraBufferRef causes a VIDIOC_QBUF ioctl to the Camera's V4L2 fd.
        preview_cref_ = nullptr;

        vh_ = nullptr;
    }
}

#if 0
void App::SignalTerminate(int signal)
{
    vacon::gShuttingDown = true;

    static unsigned user_anger = 0;
    if ((signal == SIGINT || signal == SIGTERM) && ++user_anger > 1) {
        puts("\n\nUser anger detected, exiting immediately !!!\n");
        _exit(EXIT_FAILURE);
    }

    // Signal threads to stop.
    for (auto& thread : vacon::gApp.threads) {
        thread.request_stop();
    }
}

static void SignalUSR1([[maybe_unused]] int signal = 0)
{
    vacon::gUSR1 = 1;
}

static void SetupSignals(const bool want_sigusr1)
{
    std::signal(SIGINT, App::SignalTerminate);
    std::signal(SIGTERM, App::SignalTerminate);
    if (want_sigusr1) {
        PLOG_DEBUG << "Send SIGUSR1 to simulate a packet drop";
        std::signal(SIGUSR1, SignalUSR1);
    }
}
#endif

void App::StartNetworkHandler()
{
    if (nh_) {
        return;
    }

    // Get network parameters.
    auto params = NetworkHandlerParams {
        .signaling_base_url             = args_.get<std::string>("--network-signaling-url"),
        .signaling_secret               = args_.get<std::string>("--network-signaling-secret"),
        .stun_server                    = args_.get<std::string>("--network-stun-server"),
        .outgoing_video_packet_queue    = outgoing_video_packet_queue_,
    };

    nh_ = NetworkHandler::Create(params);
    nh_->Init();
    nh_->StartAsync();
}

void App::StopNetworkHandler()
{
    nh_ = nullptr;
}

} // namespace vacon
