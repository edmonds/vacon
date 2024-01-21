// Copyright (c) 2024 The Vacon Authors
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

#pragma once

#include <csignal>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <argparse/argparse.hpp>
#include <readerwritercircularbuffer.h>

#include "common.hpp"
#include "linux/video_frame.hpp"
#include "linux/video_handler.hpp"
#include "network_handler.hpp"

namespace vacon {

typedef moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<linux::VideoFrame>>
    VideoPacketQueue;

struct App {
    public:
        static void SignalTerminate(int signal = 0);

        bool Setup(int argc, char *argv[]);
        void Shutdown();

        void StartNetworkHandler();
        void StartVideoHandler();

        void StartNetworkHandlerBackground();
        void StartVideoHandlerBackground();

        void StopNetworkHandler();
        void StopVideoHandler();

        int verbosity = 0;

        argparse::ArgumentParser                args = argparse::ArgumentParser("vacon");
        std::shared_ptr<VideoPacketQueue>       outgoing_video_packet_queue = nullptr;
        std::unique_ptr<NetworkHandler>         nh = nullptr;
        std::unique_ptr<linux::VideoHandler>    vh = nullptr;
        std::vector<std::jthread>               threads = {};

    private:
        void ParseArgs(int argc, char *argv[]);
};

extern struct App gApp;

extern volatile std::sig_atomic_t gShuttingDown;
extern volatile std::sig_atomic_t gUSR1;

} // namespace vacon
