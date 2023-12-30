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

#include <argparse/argparse.hpp>
#include <backward.hpp>
#include <plog/Log.h>
#include <readerwritercircularbuffer.h>

#include "common.hpp"
#include "camera_encoder.hpp"
#include "network_handler.hpp"
#include "vpacket.hpp"

using std::string;

using namespace std::chrono_literals;

namespace vacon {

backward::SignalHandling gBackwardSignalHandling;

volatile std::sig_atomic_t gSignalUSR1;
volatile std::sig_atomic_t gShuttingDown;

static argparse::ArgumentParser args("vacon");

static const char *kDefaultCameraDevice             = "/dev/video0";
static const char *kDefaultCameraCodec              = "hevc_vaapi";
static const char *kDefaultCameraPixelFormat        = "";
static const char *kDefaultCameraEncoderPixelFormat = "p010";
static const int kDefaultCameraWidth                = 1920;
static const int kDefaultCameraHeight               = 1080;
static const int kDefaultCameraFrameRate            = 60;
static const int kDefaultCameraBitrateKbps          = 10'000;
static const char *kDefaultSignalingUrl             = "ws://127.0.0.1:8000/v1/ooo";
static const char *kDefaultStunServer               = "stun:stun.l.google.com:19302";

static int verbosity = 0;

static std::vector<std::jthread> threads = {};

static moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<VPacket>>
    gOutgoingCameraPacketBuffer(1);

static void parseArgs(int argc, char *argv[])
{
    args.add_argument("-v", "--verbose")
        .help("increase logging verbosity")
        .action([](const auto &) { ++verbosity; })
        .append()
        .default_value(false)
        .implicit_value(true)
        .nargs(0);

    args.add_argument("-c", "--camera")
        .help("start camera encoder")
        .flag();

    args.add_argument("--camera-device")
        .metavar("DEVICE")
        .help("camera device node")
        .default_value(kDefaultCameraDevice)
        .nargs(1);

    args.add_argument("--camera-codec")
        .metavar("CODEC")
        .help("camera encoder codec")
        .default_value(kDefaultCameraCodec)
        .nargs(1);

    args.add_argument("--camera-pixel-format")
        .metavar("FMT")
        .help("camera capture pixel format")
        .default_value(kDefaultCameraPixelFormat)
        .nargs(1);

    args.add_argument("--camera-encoder-pixel-format")
        .metavar("FMT")
        .help("camera encoder pixel format")
        .default_value(kDefaultCameraEncoderPixelFormat)
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

    args.add_argument("--camera-bitrate")
        .metavar("K")
        .help("camera codec encoder bitrate (Kbps)")
        .default_value(kDefaultCameraBitrateKbps)
        .scan<'i', int>()
        .nargs(1);

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
        fmt::println(stderr, "vacon: Error parsing arguments: {}", err.what());
        exit(EXIT_FAILURE);
    }
}

static void signalTerminate(int signal __attribute__((unused)))
{
    gShuttingDown = true;
    // Signal threads to stop.
    for (auto& thread : threads) {
        thread.request_stop();
    }
}

static void signalUSR1(int signal __attribute__((unused)))
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
    setupLogging(verbosity);
    setupSignals(args["--usr1"] == true);

    if (!setupRealtimePriority()) {
        PLOG_ERROR << "Unable to set real-time thread priority, performance may be affected!";
    }

    // Start network handler on main thread (actual work is done on
    // libdatachannel thread pool).
    std::unique_ptr<NetworkHandler> nh = nullptr;
    if (args["--network"] == true) {
        PLOG_DEBUG << "Starting network handler";

        auto params = NetworkHandlerParams {
            .signaling_base_url         = args.get<string>("--network-signaling-url"),
            .signaling_secret           = args.get<string>("--network-signaling-secret"),
            .stun_server                = kDefaultStunServer,
        };

        nh = NetworkHandler::Create(params);
        nh->connectWebRTC();
    } else {
        // If the network handler is disabled, start a thread to drain and discard
        // the camera packet buffer.
        threads.emplace_back(std::jthread { [](std::stop_token st) {
            PLOG_DEBUG << "Starting camera packet buffer drain thread";
            setThreadName("VCameraDrain");

            while (!st.stop_requested()) {
                std::shared_ptr<VPacket> pkt;
                if (gOutgoingCameraPacketBuffer.wait_dequeue_timed(pkt, 250ms)) {
                    PLOG_DEBUG << fmt::format("Dequeued packet @ {}", fmt::ptr(pkt->ptr));
                }
            }
            PLOG_DEBUG << "Stopping camera packet buffer drain thread";
        }});
    }

    // Start camera encoder thread.
    if (args["--camera"] == true) {
        threads.emplace_back(std::jthread { [&](std::stop_token st) {
            PLOG_DEBUG << "Starting camera encoder thread";
            setThreadName("VCameraEncoder");

            auto params = CameraEncoderParams {
                .device                 = args.get<string>("--camera-device"),
                .codec                  = args.get<string>("--camera-codec"),
                .camera_pixel_format    = args.get<string>("--camera-pixel-format"),
                .encoder_pixel_format   = args.get<string>("--camera-encoder-pixel-format"),
                .width                  = args.get<int>("--camera-width"),
                .height                 = args.get<int>("--camera-height"),
                .frame_rate             = args.get<int>("--camera-frame-rate"),
                .bitrate                = args.get<int>("--camera-bitrate"),
            };

            auto ce = CameraEncoder::Create(params);
            if (ce) {
                ce->encodePackets(st, [st](std::shared_ptr<VPacket> pkt) {
                    PLOG_VERBOSE << fmt::format("Got an encoded packet @ {}, size {}", fmt::ptr(pkt->ptr), pkt->ptr->size);
                    while (!st.stop_requested()) {
                        if (gOutgoingCameraPacketBuffer.wait_enqueue_timed(pkt, 250ms)) {
                            break;
                        }
                    }

                });
            } else {
                PLOG_FATAL << "Failed to create camera encoder";
            }

            PLOG_DEBUG << fmt::format("Stopping camera encoder thread");
        }});
    }

    if (nh) {
        while (!nh->isConnected() && !gShuttingDown) {
            std::this_thread::sleep_for(5ms);
        }
        PLOG_FATAL << "READY !!!";
    }

    while (!gShuttingDown) {
        std::this_thread::sleep_for(500ms);
    }

    // Join threads.
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    return EXIT_SUCCESS;
}

} // namespace vacon

int main(int argc, char *argv[])
{
    return vacon::main(argc, argv);
}
