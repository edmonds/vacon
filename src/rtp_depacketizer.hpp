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

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <memory>

#include <rtc/rtc.hpp>
#include <readerwriterqueue.h>

#include "common.hpp"

namespace vacon {

class RtpDepacketizer {
    public:
        static std::unique_ptr<RtpDepacketizer> Create();
        RtpDepacketizer(RtpDepacketizer&&) = default;
        ~RtpDepacketizer();

        void submitRtpPacket(rtc::binary packet);

        AVFormatContext *fctx;

    private:
        RtpDepacketizer() = default;
        bool initFfmpeg();

        static int readAvioPacketString(void *opaque, uint8_t *buf, int buf_size);
        static int readAvioPacketRTP(void *opaque, uint8_t *buf, int buf_size);
        static int writeAvioPacketRTP(void *opaque, uint8_t *buf, int buf_size);

        // Fixed size buffer for readAvioPacketRTP() to write into.
        std::array<std::byte, 1500> buf;

        // Queue for incoming RTP packets.
        moodycamel::BlockingReaderWriterQueue<rtc::binary> rtp_packet_queue;

        AVIOContext *rtp_ioctx;
        AVIOContext *sdp_ioctx;

        int count_rtp_packets = -1;
        int count_rtp_bytes = 0;
        std::chrono::time_point<std::chrono::steady_clock> t_last;
};

} // namespace vacon
