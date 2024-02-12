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

#include "packet_ref.hpp"

#include <memory>

extern "C" {
#include <libavcodec/packet.h>
}

namespace vacon {

std::shared_ptr<PacketRef> PacketRef::Create(AVPacket* packet)
{
    return std::make_shared<PacketRef>(PacketRef(packet));
}

PacketRef::PacketRef(PacketRef&& src)
    : packet_(src.packet_)
{
    src.packet_ = nullptr;
}

PacketRef::~PacketRef()
{
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }
}

} // namespace vacon
