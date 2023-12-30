#pragma once

#include <array>
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
};

} // namespace vacon
