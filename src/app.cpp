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

#include <csignal>
#include <format>

#include <SDL3/SDL.h>
#include <plog/Log.h>

#include "event.hpp"
#include "invite.hpp"
#include "linux/camera.hpp"
#include "linux/decoder.hpp"
#include "linux/encoder.hpp"
#include "network_handler.hpp"
#include "util.hpp"

namespace vacon {

volatile std::sig_atomic_t gShuttingDown;
volatile std::sig_atomic_t gUSR1;

static void SignalUSR1([[maybe_unused]] int signal)
{
    vacon::gUSR1 = 1;
}

int App::AppInit(int argc, char *argv[])
{
    ParseArgs(argc, argv);

    util::SetupLogging(verbosity_);

#if defined(VACON_ONEVPL_PRIORITY_PATH)
    if (!getenv("ONEVPL_PRIORITY_PATH")) {
        LOG_DEBUG << std::format("Setting environment variable ONEVPL_PRIORITY_PATH = '{}'",
                                 VACON_ONEVPL_PRIORITY_PATH);
        setenv("ONEVPL_PRIORITY_PATH", VACON_ONEVPL_PRIORITY_PATH, 1 /* overwrite */);
    }
#endif

    if (args_["--usr1"] == true) {
        LOG_DEBUG << "Send SIGUSR1 to simulate a packet drop";
        std::signal(SIGUSR1, SignalUSR1);
    }

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

    auto invite_str = args_.get<std::string>("invite");
    if (invite_str != "") {
        invite_ = Invite::Decode(invite_str);
        if (!invite_) {
            LOG_FATAL << "Unable to decode invite: " << invite_str;
            return -1;
        }
        CreateConference();
    }

    return 0;
}

void App::AppQuit()
{
    StopVideo();
}

int App::AppEvent(const SDL_Event *event)
{
    ProcessUiEvent(event);

    switch (event->type) {
    case SDL_EVENT_QUIT: {
        return ShutdownEvent();
    }

    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
        if (event->window.windowID == SDL_GetWindowID(sdl_window_)) {
            return ShutdownEvent();
        }
        break;
    }

    case SDL_EVENT_USER: {
        ProcessUserEvent(&event->user);
        break;
    }

    case SDL_EVENT_KEY_UP: {
        auto key = &event->key.keysym;
        switch (key->sym) {
            case SDLK_d:
                if (key->mod & (SDL_KMOD_ALT | SDL_KMOD_SHIFT)) {
                    xxx_enable_imgui_demo_window_ = !xxx_enable_imgui_demo_window_;
                }
            break;

            case SDLK_q:
                if (key->mod & (SDL_KMOD_ALT | SDL_KMOD_SHIFT)) {
                    return ShutdownEvent();
                }
            break;
        }
        break;
    }

    default:
        // Ignore.
        break;
    }

    return 0;
}

int App::ShutdownEvent()
{
    LOG_INFO << "App shutdown requested!";
    vacon::gShuttingDown = true;
    return 1;
}

void App::ProcessUserEvent(const SDL_UserEvent *user)
{
    auto event = static_cast<Event>(user->code);

    switch (event) {

    case Event::CameraStarting:
        LOG_DEBUG << "[CameraStarting]";
        break;

    case Event::CameraStarted:
        n_camera_timeouts_ = 0;
        if (sdl_renderer_) {
            LOG_DEBUG << "[CameraStarted] Calling Camera::ExportBuffersToOpenGL() on render thread";
            camera_->ExportBuffersToOpenGL(sdl_renderer_);
        }
        LOG_DEBUG << "[CameraStarted] Starting video encoder";
        StartVideoEncoder();
        break;

    case Event::CameraFailed: {
        LOG_FATAL << "[CameraFailed]";
        const int MAX_CAMERA_TIMEOUTS = 3;
        if (n_camera_timeouts_ < MAX_CAMERA_TIMEOUTS) {
            StartVideoCamera();
        }
        break;
    }

    case Event::CameraTimeout:
        LOG_DEBUG << "[CameraTimeout]";
        ++n_camera_timeouts_;
        break;

    case Event::DecoderStarting:
        LOG_DEBUG << "[DecoderStarting]";
        break;

    case Event::DecoderStarted:
        LOG_DEBUG << "[DecoderStarted]";
        break;

    case Event::DecoderFailed:
        LOG_FATAL << "[DecoderFailed]";
        break;

    case Event::EncoderStarting:
        LOG_DEBUG << "[EncoderStarting]";
        break;

    case Event::EncoderStarted:
        LOG_DEBUG << "[EncoderStarted]";
        break;

    case Event::EncoderFailed:
        LOG_FATAL << "[EncoderFailed]";
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

void App::StartNetworkHandler()
{
    if (nh_ || !invite_) {
        LOG_FATAL << "NetworkHandler or Invite not set, cannot start network handler";
        return;
    }

    // Get network parameters.
    auto params = NetworkHandlerParams {
        .invite                         = invite_,
        .stun_server                    = args_.get<std::string>("--network-stun-server"),
        .outgoing_video_packet_queue    = outgoing_video_packet_queue_,
        .incoming_video_packet_queue    = incoming_video_packet_queue_,
    };

    // Start the NetworkHandler.
    nh_ = NetworkHandler::Create(params);
    if (nh_) {
        nh_->Init();
        nh_->StartAsync();
    } else {
        LOG_DEBUG << "NetworkHandler::Create() failed";
    }
}

void App::StopNetworkHandler()
{
    nh_ = nullptr;
    invite_ = nullptr;
}

void App::StartVideo()
{
    if (camera_) {
        return;
    }

    StartVideoCamera();
    StartVideoDecoder();
}

void App::StartVideoCamera()
{
    camera_ = linux::Camera::Create(linux::CameraParams {
        .device         = args_.get<std::string>("--camera-device"),
        .encoder_queue  = encoder_queue_,
        .preview_queue  = preview_queue_,
    });
    if (!camera_) {
        LOG_FATAL << "linux::Camera::Create() failed!";
        return;
    }
    camera_->Init();
}

void App::StartVideoDecoder()
{
    decoder_ = linux::Decoder::Create(linux::DecoderParams {
        .incoming_video_packet_queue    = incoming_video_packet_queue_,
        .decoded_video_frame_queue      = decoded_video_frame_queue_,
    });
    if (!decoder_) {
        LOG_FATAL << "linux::Decoder::Create() failed!";
        return;
    }
    decoder_->Init();
}

void App::StartVideoEncoder()
{
    encoder_ = linux::Encoder::Create(linux::EncoderParams {
        .camera_format                  = camera_->GetCameraFormat(),
        .bitrate_kbps                   = args_.get<unsigned>("--video-encoder-bitrate"),
        .encoder_queue                  = encoder_queue_,
        .outgoing_video_packet_queue    = outgoing_video_packet_queue_,
    });
    if (!encoder_) {
        LOG_FATAL << "[CameraStarted] linux::Encoder::Create() failed!";
        return;
    }
    encoder_->Init();
}

void App::StopVideo()
{
    StopNetworkHandler();

    // Signal the background threads to stop.
    if (camera_)  { camera_ ->RequestStop(); }
    if (decoder_) { decoder_->RequestStop(); }
    if (encoder_) { encoder_->RequestStop(); }

    // Wait for the background threads to stop.
    if (camera_)  { camera_ ->Join(); }
    if (decoder_) { decoder_->Join(); }
    if (encoder_) { encoder_->Join(); }

    // Drain the video queues.
    while (encoder_queue_->try_pop()) {}
    while (preview_queue_->try_pop()) {}
    while (decoded_video_frame_queue_->try_pop()) {}
    while (outgoing_video_packet_queue_->try_pop()) {}
    while (incoming_video_packet_queue_->try_pop()) {}

    // Free the cached frames that depend on resources allocated by the video
    // objects.
    decoded_frame_  = nullptr;
    preview_cref_   = nullptr;

    // Free the video objects.
    camera_     = nullptr;
    decoder_    = nullptr;
    encoder_    = nullptr;
}

void App::CreateConference()
{
    if (!invite_) {
        invite_ = Invite::Create(InviteParams {
            .signaling_server   = vacon::kAppDefaultSignalingServer,
            .description        = std::string(""),
        });
    }

    if (invite_) {
        LOG_INFO << "Creating conference using invite " << invite_->Encode();
        StartNetworkHandler();
        StartVideo();
    } else {
        LOG_FATAL << "Invite::Create() failed!";
    }
}

} // namespace vacon
