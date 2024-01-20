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

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <argparse/argparse.hpp>
#include <plog/Log.h>
#include <readerwritercircularbuffer.h>

#include "common.hpp"
#include "linux/video_handler.hpp"
#include "network_handler.hpp"

using std::string;

using namespace std::chrono_literals;

namespace vacon {

static argparse::ArgumentParser args("vacon");
static int gVerbosity = 0;
static std::vector<std::jthread> gThreads = {};
volatile std::sig_atomic_t gSignalUSR1;
volatile std::sig_atomic_t gShuttingDown;

static const char *kDefaultCameraDevice             = "/dev/video0";
static const char *kDefaultCameraPixelFormat        = "";
static const int kDefaultCameraWidth                = 1920;
static const int kDefaultCameraHeight               = 1080;
static const int kDefaultCameraFrameRate            = 60;
static const int kDefaultVideoEncoderBitrateKbps    = 10'000;
static const char *kDefaultSignalingUrl             = "ws://127.0.0.1:8000/v1/ooo";
static const char *kDefaultStunServer               = "stun:stun.l.google.com:19302";

static void parseArgs(int argc, char *argv[])
{
    args.add_argument("-v", "--verbose")
        .help("increase logging verbosity")
        .action([](const auto &) { ++gVerbosity; })
        .append()
        .default_value(false)
        .implicit_value(true)
        .nargs(0);

    args.add_argument("-c", "--camera")
        .help("start camera capture")
        .flag();

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

    args.add_argument("-e", "--video-encoder")
        .help("start video encoder")
        .flag();

    args.add_argument("--video-encoder-bitrate")
        .metavar("K")
        .help("video encoder bitrate (Kbps)")
        .default_value(kDefaultVideoEncoderBitrateKbps)
        .scan<'i', int>()
        .nargs(1);

    args.add_argument("-p", "--player")
        .help("start video player (requires network handler)")
        .flag();

    args.add_argument("-n", "--network")
        .help("start network handler")
        .flag();

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

static void signalTerminate(int signal = 0)
{
    static unsigned user_anger = 0;
    if ((signal == SIGINT || signal == SIGTERM) && ++user_anger > 1) {
        puts("\n\nUser anger detected, exiting immediately !!!\n");
        _exit(EXIT_FAILURE);
    }

    gShuttingDown = true;

    // Signal the plplay thread to shut down. This is safe to call even if the
    // plplay thread wasn't started.
    plplay_shutdown();

    // Signal threads to stop.
    for (auto& thread : gThreads) {
        thread.request_stop();
    }
}

static void signalUSR1(int signal __attribute__((unused)) = 0)
{
    gSignalUSR1 = 1;
}

static void setupSignals(const bool want_sigusr1)
{
    std::signal(SIGINT, signalTerminate);
    std::signal(SIGTERM, signalTerminate);
    if (want_sigusr1) {
        PLOG_DEBUG << "Send SIGUSR1 to simulate a packet drop";
        std::signal(SIGUSR1, signalUSR1);
    }
}

int main(int argc, char *argv[])
{
    parseArgs(argc, argv);
    setupLogging(gVerbosity);
    setupSignals(args["--usr1"] == true);

    if (!setupRealtimePriority()) {
        PLOG_ERROR << "Unable to set real-time thread priority, performance may be affected!";
    }

    // Start the network handler.
    std::unique_ptr<NetworkHandler> nh = nullptr;
    if (args["--network"] == true) {
        PLOG_DEBUG << "Starting network handler";

        auto params = NetworkHandlerParams {
            .signaling_base_url         = args.get<string>("--network-signaling-url"),
            .signaling_secret           = args.get<string>("--network-signaling-secret"),
            .stun_server                = kDefaultStunServer,
        };

        nh = NetworkHandler::Create(params);
        nh->Init();

        // Start connecting to the signaling server and the WebRTC peer.
        nh->ConnectWebRTC();
    }

    // Get camera parameters.
    std::optional<linux::CameraParams> camera_params = {};
    if (args["--camera"] == true) {
        camera_params = { linux::CameraParams {
            .device         = args.get<string>("--camera-device"),
            .pixel_format   = args.get<string>("--camera-pixel-format"),
            .width          = args.get<int>("--camera-width"),
            .height         = args.get<int>("--camera-height"),
            .frame_rate     = args.get<int>("--camera-frame-rate"),
        }};
    }

    // Get video encoder parameters.
    std::optional<linux::EncoderParams> encoder_params = {};
    if (args["--video-encoder"] == true) {
        encoder_params = { linux::EncoderParams {
            .input_pixel_format     = args.get<string>("--camera-pixel-format"),
            .width                  = args.get<int>("--camera-width"),
            .height                 = args.get<int>("--camera-height"),
            .frame_rate             = args.get<int>("--camera-frame-rate"),
            .bitrate_kbps           = args.get<int>("--video-encoder-bitrate"),
        }};
    }

    // Start the video handler if either the camera or the video encoder was
    // enabled.
    std::unique_ptr<linux::VideoHandler> vh = nullptr;
    if (camera_params || encoder_params) {
        PLOG_DEBUG << "Starting video handler";

        auto params = linux::VideoHandlerParams {
            .camera_params = camera_params,
            .encoder_params = encoder_params,
            .outgoing_video_packet_queue = nh ? nh->outgoing_video_packet_queue_ : nullptr,
        };

        vh = linux::VideoHandler::Create(params);

        // Start the camera and/or video encoder threads.
        vh->Init();
    }

    // Wait for the NetworkHandler to bring up the peer-to-peer connection.
    if (nh) {
        while (!nh->IsConnectedToPeer() && !gShuttingDown) {
            std::this_thread::sleep_for(5ms);
        }

        if (nh->IsConnectedToPeer() && !gShuttingDown) {
            PLOG_FATAL << "PEER-TO-PEER CONNECTION IS READY !!!";
        }

        // WebRTC peer connection is up, so close the connection to the
        // signaling server.
        nh->CloseWebSocket();
    }

    // Start video player.
    if (args["--player"] == true && nh && nh->IsConnectedToPeer()) {
        if (auto avfc = nh->GetRtpAvfcInput()) {
            // Start a thread to run plplay. This will itself start its own
            // thread to run the decode loop and then run the render loop. The
            // thread that plplay starts has to be separately signaled to shut
            // down with plplay_shutdown().
            gThreads.emplace_back(std::jthread { [avfc]() {
                PLOG_DEBUG << "Starting plplay video player thread ID " << std::this_thread::get_id();
                if (auto ret = plplay_play(avfc)) {
                    PLOG_INFO << "plplay_play() failed with return code " << ret;
                }

                // If the player exited (e.g. user pressed the escape key),
                // shut down the rest of the process.
                gShuttingDown = true;

                PLOG_DEBUG << "Stopping plplay video player thread ID " << std::this_thread::get_id();
            }});
        } else {
            PLOG_FATAL << "NetworkHandler is connected to peer, but no RTP depacketizer ?!";
        }
    }

    // Wait until it's time to shut down.
    while (!gShuttingDown) {
        if (nh && !nh->IsConnectedToPeer()) {
            PLOG_INFO << "Lost connection to peer, shutting down";
            break;
        }
        std::this_thread::sleep_for(250ms);
    }
    signalTerminate();
    if (nh) {
        nh->Stop();
    }
    if (vh) {
        vh->Stop();
    }

    // Join threads.
    PLOG_INFO << "Waiting for threads to exit...";
    for (auto& thread : gThreads) {
        if (thread.joinable()) {
            PLOG_DEBUG << "Trying to join thread ID " << thread.get_id();
            thread.join();
        } else {
            PLOG_FATAL << "Thread ID " << thread.get_id() << " is not joinable ?!";
        }
    }
    if (nh) {
        nh->Join();
    }
    if (vh) {
        vh->Join();
    }

    return EXIT_SUCCESS;
}

} // namespace vacon

int main(int argc, char *argv[])
{
    return vacon::main(argc, argv);
}
