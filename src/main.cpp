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
        std::cerr << "vacon: Error parsing arguments: " << err.what() << std::endl;
        exit(EXIT_FAILURE);
    }
}

static void signalTerminate(int signal = 0)
{
    static unsigned user_anger = 0;
    if ((signal == SIGINT || signal == SIGTERM) && ++user_anger > 1) {
        puts("\nuser anger detected !!!\n");
        _exit(EXIT_FAILURE);
    }

    gShuttingDown = true;

    // Signal the plplay thread to shut down. This is safe to call even if the
    // plplay thread wasn't started.
    plplay_shutdown();

    // Signal threads to stop.
    for (auto& thread : threads) {
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
    }

    // Start camera encoder thread.
    if (args["--camera"] == true) {
        threads.emplace_back(std::jthread { [&](std::stop_token st) {
            PLOG_DEBUG << "Starting camera encoder thread ID " << std::this_thread::get_id();
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

            PLOG_DEBUG << "Stopping camera encoder thread ID " << std::this_thread::get_id();
        }});

        // Start a thread to drain the camera packet buffer. If the network handler
        // is started, submit the packet for transmission to the peer, otherwise
        // discard it.
        threads.emplace_back(std::jthread { [&](std::stop_token st) {
            PLOG_DEBUG << "Starting camera packet buffer drain thread ID " << std::this_thread::get_id();
            setThreadName("VCameraDrain");
            while (!st.stop_requested()) {
                std::shared_ptr<VPacket> pkt;
                if (gOutgoingCameraPacketBuffer.wait_dequeue_timed(pkt, 250ms)) {
                    if (nh) {
                        nh->sendPacket(pkt);
                    }
                }
            }
            PLOG_DEBUG << "Stopping camera packet buffer drain thread ID " << std::this_thread::get_id();
        }});
    }

    // Wait for the NetworkHandler to bring up the peer-to-peer connection.
    if (nh) {
        while (!nh->isConnectedToPeer() && !gShuttingDown) {
            std::this_thread::sleep_for(5ms);
        }
        PLOG_FATAL << "READY !!!";

        // Peer-to-peer connection is up, so close the connection to the
        // signaling server.
        nh->closeWebSocket();
    }

    // Start video player.
    if (args["--player"] == true && nh && nh->isConnectedToPeer()) {
        if (auto avfc = nh->getRtpAvfcInput()) {
            // Start a thread to run plplay. This will itself start its own
            // thread to run the decode loop and then run the render loop. The
            // thread that plplay starts has to be separately signaled to shut
            // down with plplay_shutdown().
            threads.emplace_back(std::jthread { [avfc]() {
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
        if (nh && !nh->isConnectedToPeer()) {
            PLOG_INFO << "Lost connection to peer, shutting down";
            break;
        }
        std::this_thread::sleep_for(500ms);
    }

    signalTerminate();

    // Join threads.
    PLOG_INFO << "Waiting for threads to exit...";
    for (auto& thread : threads) {
        if (thread.joinable()) {
            PLOG_DEBUG << "Trying to join thread ID " << thread.get_id();
            thread.join();
        } else {
            PLOG_FATAL << "Thread ID " << thread.get_id() << " is not joinable ?!";
        }
    }

    return EXIT_SUCCESS;
}

} // namespace vacon

int main(int argc, char *argv[])
{
    return vacon::main(argc, argv);
}
