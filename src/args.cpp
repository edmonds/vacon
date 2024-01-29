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

#include <cstdlib>
#include <stdexcept>

#include <argparse/argparse.hpp>

namespace vacon {

static const char *kDefaultCameraDevice                 = "/dev/video0";
static const char *kDefaultCameraPixelFormat            = "";
static const unsigned kDefaultCameraWidth               = 1920;
static const unsigned kDefaultCameraHeight              = 1080;
static const unsigned kDefaultCameraFrameRate           = 60;
static const unsigned kDefaultVideoEncoderBitrateKbps   = 10'000;
static const char *kDefaultSignalingUrl                 = "ws://127.0.0.1:8000/v1/ooo";
//static const char *kDefaultStunServer               = "stun:stun.l.google.com:19302";

void App::ParseArgs(int argc, char *argv[])
{
    args_.add_argument("-v", "--verbose")
         .help("increase logging verbosity")
         .action([&](const auto &) { ++verbosity_; })
         .append()
         .default_value(false)
         .implicit_value(true)
         .nargs(0);

    args_.add_argument("--camera-device")
         .metavar("DEVICE")
         .help("camera device node")
         .default_value(kDefaultCameraDevice)
         .nargs(1);

    args_.add_argument("--camera-width")
         .metavar("W")
         .help("camera capture frame width")
         .default_value(kDefaultCameraWidth)
         .scan<'u', unsigned>()
         .nargs(1);

    args_.add_argument("--camera-height")
         .metavar("H")
         .help("camera capture frame height")
         .default_value(kDefaultCameraHeight)
         .scan<'u', unsigned>()
         .nargs(1);

    args_.add_argument("--camera-frame-rate")
         .metavar("R")
         .help("camera capture frame rate")
         .default_value(kDefaultCameraFrameRate)
         .scan<'u', unsigned>()
         .nargs(1);

    args_.add_argument("--camera-pixel-format")
         .metavar("FMT")
         .help("camera capture pixel format")
         .default_value(kDefaultCameraPixelFormat)
         .nargs(1);

    args_.add_argument("--video-encoder-bitrate")
         .metavar("K")
         .help("video encoder bitrate (Kbps)")
         .default_value(kDefaultVideoEncoderBitrateKbps)
         .scan<'u', unsigned>()
         .nargs(1);

    args_.add_argument("-s", "--network-signaling-secret")
         .metavar("SECRET")
         .help("signaling shared secret to identify peer")
         .required();

    args_.add_argument("-u", "--network-signaling-url")
         .metavar("URL")
         .help("signaling URL for offer/answer exchange")
         .default_value(kDefaultSignalingUrl)
         .nargs(1);

    args_.add_argument("--usr1")
         .help("setup simulated packet loss SIGUSR1 handler")
         .flag();

    args_.add_argument("--xxx-force-start-network-handler").flag();

    args_.add_argument("--xxx-force-start-video-handler").flag();

    args_.add_argument("--xxx-headless").flag();

    try {
        args_.parse_args(argc, argv);
    } catch (const std::exception& err) {
        std::cerr
            << PROJECT_NAME << ": Error parsing arguments: "
            << err.what()
            << "\n\n"
            << args_;
        exit(EXIT_FAILURE);
    }
}

} // namespace vacon
