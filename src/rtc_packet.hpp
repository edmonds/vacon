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

#include <memory>
#include <utility>

#include <rtc/rtc.hpp>

namespace vacon {

class RtcPacket {
    public:
        static std::shared_ptr<RtcPacket> Create(rtc::binary msg, rtc::FrameInfo frame_info)
        {
            return std::make_shared<RtcPacket>(RtcPacket(std::move(msg), frame_info));
        };

        RtcPacket(RtcPacket&& src)
            : frame_info_(src.frame_info_)
        {
            msg_ = std::move(src.msg_);
        };

        ~RtcPacket() = default;

        rtc::binary msg_ = {};
        rtc::FrameInfo frame_info_ = {0};

    private:
        RtcPacket(rtc::binary msg, rtc::FrameInfo frame_info)
            : msg_(std::move(msg)), frame_info_(frame_info) {};
};

} // namespace vacon
