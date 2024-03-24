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

class GenericRtpPacketizer final : public rtc::RtpPacketizer {
    public:
        inline static const uint32_t defaultClockRate = 90 * 1000;
        inline static const size_t defaultMaxFragmentSize = 1350;

        GenericRtpPacketizer(std::shared_ptr<rtc::RtpPacketizationConfig> rtpConfig,
                             uint16_t max_fragment_size = defaultMaxFragmentSize)
        : RtpPacketizer(std::move(rtpConfig)), max_fragment_size_(max_fragment_size - 1) {}

        void outgoing(rtc::message_vector& messages, const rtc::message_callback& send) override;

    private:
        const size_t max_fragment_size_;
};

} // namespace vacon
