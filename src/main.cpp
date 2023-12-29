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

#include "common.hpp"
#include "camera_encoder.hpp"

using std::string;

namespace vacon {

backward::SignalHandling gBackwardSignalHandling;

volatile std::sig_atomic_t gSignalUSR1;

static argparse::ArgumentParser args("vacon");

static const char *kDefaultCameraDevice             = "/dev/video0";
static const char *kDefaultCameraCodec              = "hevc_vaapi";
static const char *kDefaultCameraPixelFormat        = "";
static const char *kDefaultCameraEncoderPixelFormat = "p010";
static const int kDefaultCameraWidth                = 1920;
static const int kDefaultCameraHeight               = 1080;
static const int kDefaultCameraFrameRate            = 60;
static const int kDefaultCameraBitrateKbps          = 10'000;
static const char *kDefaultSignalingUrl             = "http://127.0.0.1:8000/v1/ooo";

static int verbosity = 0;

static std::optional<std::jthread> thrCameraEncoder;

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

    args.add_argument("-s", "--signaling-secret")
        .metavar("SECRET")
        .help("signaling shared secret to identify peer")
        .required();

    args.add_argument("-u", "--signaling-url")
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
    if (thrCameraEncoder) {
        thrCameraEncoder->request_stop();
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

static void runCameraEncoder(std::stop_token st)
{
    PLOG_DEBUG << "Starting camera encoder thread";

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

    ce->encodePackets(st, [](const AVPacket *pkt) {
        PLOG_DEBUG << fmt::format("Got a packet @ %p, size {}", fmt::ptr(pkt), pkt->size);
    });

    PLOG_DEBUG << fmt::format("Stopping camera encoder thread");
}

int main(int argc, char *argv[])
{
    parseArgs(argc, argv);
    setupLogging(verbosity);
    setupSignals(args["--usr1"] == true);

    if (!setupRealtimePriority()) {
        PLOG_ERROR << "Unable to set real-time thread priority, performance may be affected!";
    }

    // Construct the full signaling URL from the base URL and the shared secret.
    string signalingUrl = fmt::format("{}/{}",
                                      args.get<string>("--signaling-url"),
                                      args.get<string>("--signaling-secret"));
    PLOG_DEBUG << fmt::format("signalingUrl = {}", signalingUrl);

    if (args["--camera"] == true) {
        thrCameraEncoder = std::jthread(runCameraEncoder);
    }

    if (thrCameraEncoder) {
        thrCameraEncoder->join();
    }

    return EXIT_SUCCESS;
}

} // namespace vacon

int main(int argc, char *argv[])
{
    return vacon::main(argc, argv);
}
