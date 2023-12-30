#pragma once

#include "common.hpp"

extern "C" {
#include <libavcodec/packet.h>
}

namespace vacon {

struct VPacket {
    AVPacket *ptr = nullptr;

    VPacket(AVPacket **packet)
    {
        ptr = *packet;
        *packet = nullptr;
        PLOG_DEBUG << fmt::format("Taking ownership of AVPacket @ {}", fmt::ptr(ptr));
    }

    VPacket(VPacket&& src_packet)
    {
        ptr = src_packet.ptr;
        src_packet.ptr = nullptr;
    }

    ~VPacket()
    {
        PLOG_DEBUG << fmt::format("Destroying AVPacket @ {}", fmt::ptr(ptr));
        av_packet_free(&ptr);
    }
};

} // namespace vacon
