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

#include <cstdint>
#include <vector>

#include <rtc/rtc.hpp>

namespace vacon {

class GenericRtpDepacketizer : public rtc::MediaHandler {
    public:
        GenericRtpDepacketizer() = default;
        virtual ~GenericRtpDepacketizer() = default;

        void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;

    private:
        std::vector<rtc::message_ptr> rtp_buffer_;

        rtc::message_vector ReassemblePackets(rtc::message_vector::iterator first_frag,
                                              rtc::message_vector::iterator last_frag,
                                              uint32_t timestamp);
};

} // namespace vacon
