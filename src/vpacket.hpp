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
        PLOG_VERBOSE << fmt::format("Taking ownership of AVPacket @ {}", fmt::ptr(ptr));
    }

    VPacket(VPacket&& src_packet)
    {
        ptr = src_packet.ptr;
        src_packet.ptr = nullptr;
    }

    ~VPacket()
    {
        PLOG_VERBOSE << fmt::format("Destroying AVPacket @ {}", fmt::ptr(ptr));
        av_packet_free(&ptr);
    }
};

} // namespace vacon
