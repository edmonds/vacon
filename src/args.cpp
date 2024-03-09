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
static const unsigned kDefaultVideoEncoderBitrateKbps   = 10'000;
static const char *kDefaultStunServer                   = "stun:stun.l.google.com:19302";

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

    args_.add_argument("--video-encoder-bitrate")
         .metavar("K")
         .help("video encoder bitrate (Kbps)")
         .default_value(kDefaultVideoEncoderBitrateKbps)
         .scan<'u', unsigned>()
         .nargs(1);

    args_.add_argument("--network-stun-server")
         .metavar("STUN-URL")
         .help("STUN server to use")
         .default_value(kDefaultStunServer)
         .nargs(1);

    args_.add_argument("--usr1")
         .help("setup simulated packet loss SIGUSR1 handler")
         .flag();

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
