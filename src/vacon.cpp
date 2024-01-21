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

#include "vacon.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "common.hpp"
#include "linux/video_handler.hpp"
#include "network_handler.hpp"

using namespace std::chrono_literals;

namespace vacon {

struct App gApp = {};

volatile std::sig_atomic_t gShuttingDown = false;
volatile std::sig_atomic_t gUSR1 = false;

static const char *kDefaultCameraDevice             = "/dev/video0";
static const char *kDefaultCameraPixelFormat        = ""; // XXX Default value crashes, TODO V4L2 autodetection
static const int kDefaultCameraWidth                = 1920;
static const int kDefaultCameraHeight               = 1080;
static const int kDefaultCameraFrameRate            = 60;
static const int kDefaultVideoEncoderBitrateKbps    = 10'000;
static const char *kDefaultSignalingUrl             = "ws://127.0.0.1:8000/v1/ooo";
static const char *kDefaultStunServer               = "stun:stun.l.google.com:19302";

void App::Shutdown()
{
    // Drain any video frames remaining on the outgoing video packet queue.
    if (outgoing_video_packet_queue) {
        std::shared_ptr<linux::VideoFrame> frame = {};
        while (outgoing_video_packet_queue->try_dequeue(frame)) {
            PLOG_VERBOSE << fmt::format("Drained VideoFrame @ {}", fmt::ptr(frame.get()));
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

void App::ParseArgs(int argc, char *argv[])
{
    args.add_argument("-v", "--verbose")
        .help("increase logging verbosity")
        .action([&](const auto &) { ++verbosity; })
        .append()
        .default_value(false)
        .implicit_value(true)
        .nargs(0);

    args.add_argument("--camera-device")
        .metavar("DEVICE")
        .help("camera device node")
        .default_value(kDefaultCameraDevice)
        .nargs(1);

    args.add_argument("--camera-width")
        .metavar("W")
        .help("camera capture frame width")
        .default_value(kDefaultCameraWidth)
        .scan<'i', int>()
        .nargs(1);

    args.add_argument("--camera-height")
        .metavar("H")
        .help("camera capture frame height")
        .default_value(kDefaultCameraHeight)
        .scan<'i', int>()
        .nargs(1);

    args.add_argument("--camera-frame-rate")
        .metavar("R")
        .help("camera capture frame rate")
        .default_value(kDefaultCameraFrameRate)
        .scan<'i', int>()
        .nargs(1);

    args.add_argument("--camera-pixel-format")
        .metavar("FMT")
        .help("camera capture pixel format")
        .default_value(kDefaultCameraPixelFormat)
        .nargs(1);

    args.add_argument("--video-encoder-bitrate")
        .metavar("K")
        .help("video encoder bitrate (Kbps)")
        .default_value(kDefaultVideoEncoderBitrateKbps)
        .scan<'i', int>()
        .nargs(1);

    args.add_argument("-s", "--network-signaling-secret")
        .metavar("SECRET")
        .help("signaling shared secret to identify peer")
        .required();

    args.add_argument("-u", "--network-signaling-url")
        .metavar("URL")
        .help("signaling URL for offer/answer exchange")
        .default_value(kDefaultSignalingUrl)
        .nargs(1);

    args.add_argument("--usr1")
        .help("setup simulated packet loss SIGUSR1 handler")
        .flag();

    args.add_argument("--xxx-force-start-network-handler").flag();

    args.add_argument("--xxx-force-start-video-handler").flag();

    try {
        args.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr
            << "vacon: Error parsing arguments: "
            << err.what()
            << std::endl << std::endl
            << args;
        exit(EXIT_FAILURE);
    }
}

void App::StopNetworkHandler()
{
    if (nh) {
        nh = nullptr;
    }
}

void App::StopVideoHandler()
{
    if (vh) {
        vh = nullptr;
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

void App::StartVideoHandler()
{
    if (vh) {
        return;
    }

    // Get camera parameters.
    std::optional<linux::CameraParams> camera_params = {
        linux::CameraParams {
            .device         = args.get<std::string>("--camera-device"),
            .pixel_format   = args.get<std::string>("--camera-pixel-format"),
            .width          = args.get<int>("--camera-width"),
            .height         = args.get<int>("--camera-height"),
            .frame_rate     = args.get<int>("--camera-frame-rate"),
        }
    };

    // Get video encoder parameters.
    std::optional<linux::EncoderParams> encoder_params = {
        linux::EncoderParams {
            .input_pixel_format = args.get<std::string>("--camera-pixel-format"),
            .width              = args.get<int>("--camera-width"),
            .height             = args.get<int>("--camera-height"),
            .frame_rate         = args.get<int>("--camera-frame-rate"),
            .bitrate_kbps       = args.get<int>("--video-encoder-bitrate"),
        }
    };

    // Start the video handler.
    PLOG_DEBUG << "Starting video handler";

    auto params = linux::VideoHandlerParams {
        .camera_params = camera_params,
        .encoder_params = encoder_params,
        .outgoing_video_packet_queue = outgoing_video_packet_queue,
    };

    vh = linux::VideoHandler::Create(params);

    // Start the camera and video encoder threads.
    vh->Init();
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

} // namespace vacon
