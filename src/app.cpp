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
//#include "network_handler.hpp"
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
    outgoing_video_packet_queue = std::make_shared<VideoPacketQueue>(2);

    if (args["--xxx-force-start-network-handler"] == true) {
        StartNetworkHandlerBackground();
    }

    if (args["--xxx-force-start-video-handler"] == true) {
        StartVideoHandlerBackground();
    }
#endif

#if 0
    if (args_["--mirror"] == true) {
        sdl_renderer_flip_ = SDL_FLIP_HORIZONTAL;
    }

    camera_ = Camera::Create(CameraParams {
        .device         = args_.get<std::string>("--device"),
        .pixel_format   = args_.get<std::string>("--pixel-format"),
        .frame_rate     = args_.get<int>("--frame-rate"),
        .width          = args_.get<int>("--width"),
        .height         = args_.get<int>("--height"),
    });
    if (!camera_) {
        LOG_FATAL << "Camera::Create() failed";
        return -1;
    }

    if (!camera_->Init()) {
        LOG_FATAL << "Camera::Init() failed";
        return -1;
    }

    if (!InitSDL()) {
        LOG_FATAL << "App::InitSDL() failed";
        return -1;
    }

    if (!camera_->ExportBuffersToOpenGL(sdl_renderer_)) {
        LOG_FATAL << "Camera::ExportBuffers() failed";
        return -1;
    }

    encoder_ = Encoder::Create(EncoderParams {
        .pixel_format   = util::FourCcToString(camera_->fmt_.pixelformat),
        .width          = camera_->fmt_.width,
        .height         = camera_->fmt_.height,
        .frame_rate     = static_cast<unsigned>(args_.get<int>("--frame-rate")),
        .bitrate_kbps   = 5000,
    });
    if (!encoder_) {
        LOG_FATAL << "Encoder::Create() failed";
        return -1;
    }
    if (!encoder_->Init()) {
        LOG_FATAL << "Encoder::Init() failed";
        return -1;
    }

    if (!encoder_->Init2()) {
        LOG_FATAL << "Encoder::Init() failed";
        return -1;
    }

    if (!camera_->StartCapturing()) {
        LOG_FATAL << "Camera::StartCapturing() failed";
        return -1;
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

#if 0
    // Stop the handlers.
    if (nh) {
        nh->Stop();
    }
    if (vh) {
        vh->Stop();
    }

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
    if (nh) {
        nh->Join();
        nh = nullptr;
    }
    if (vh) {
        vh->Join();
        vh = nullptr;
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
void App::Shutdown()
{
    // Drain any video frames remaining on the outgoing video packet queue.
    if (outgoing_video_packet_queue) {
        std::shared_ptr<linux::VideoFrame> frame = {};
        while (outgoing_video_packet_queue->try_dequeue(frame)) {
            PLOG_VERBOSE << std::format("Drained VideoFrame @ {}", std::ptr(frame.get()));
        }
    }
    outgoing_video_packet_queue = nullptr;

    // Stop the handlers.
    if (nh) {
        nh->Stop();
    }
    if (vh) {
        vh->Stop();
    }

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
    if (nh) {
        nh->Join();
        nh = nullptr;
    }
    if (vh) {
        vh->Join();
        vh = nullptr;
    }
}

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

void App::StopNetworkHandler()
{
    if (nh) {
        nh = nullptr;
    }
}

void App::StartNetworkHandler()
{
    if (nh) {
        return;
    }

    // Start the network handler.
    PLOG_DEBUG << "Starting network handler";

    auto params = NetworkHandlerParams {
        .signaling_base_url             = args.get<std::string>("--network-signaling-url"),
        .signaling_secret               = args.get<std::string>("--network-signaling-secret"),
        .stun_server                    = kDefaultStunServer,
        .outgoing_video_packet_queue    = outgoing_video_packet_queue,
    };

    nh = NetworkHandler::Create(params);
    nh->Init();

    // Start connecting to the signaling server and the WebRTC peer.
    nh->ConnectWebRTC();

    // Wait for the NetworkHandler to bring up the peer-to-peer connection.
    while (!nh->IsConnectedToPeer() && !vacon::gShuttingDown) {
        std::this_thread::sleep_for(5ms);
    }

    if (nh->IsConnectedToPeer() && !vacon::gShuttingDown) {
        PLOG_FATAL << "PEER-TO-PEER CONNECTION IS READY !!!";
    }

    // WebRTC peer connection is up, so close the connection to the
    // signaling server.
    nh->CloseWebSocket();
}

void App::StartNetworkHandlerBackground()
{
    std::jthread([&]() {
        StartNetworkHandler();
    }).detach();
}

void App::StartVideoHandlerBackground()
{
    std::jthread([&]() {
        StartVideoHandler();
    }).detach();
}

bool App::Setup(int argc, char *argv[])
{
    ParseArgs(argc, argv);

    setupLogging(verbosity);

    SetupSignals(args["--usr1"] == true);
    if (!setupRealtimePriority()) {
        PLOG_ERROR << "Unable to set real-time thread priority, performance may be affected!";
    }

    outgoing_video_packet_queue = std::make_shared<VideoPacketQueue>(2);

    if (args["--xxx-force-start-network-handler"] == true) {
        StartNetworkHandlerBackground();
    }

    if (args["--xxx-force-start-video-handler"] == true) {
        StartVideoHandlerBackground();
    }

    return true;
}
#endif

} // namespace vacon
