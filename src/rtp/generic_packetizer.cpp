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

#include "rtp/generic_packetizer.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <iterator>
#include <memory>

#include <rtc/rtc.hpp>

namespace vacon {

void GenericRtpPacketizer::outgoing(rtc::message_vector& messages,
                                    [[maybe_unused]] const rtc::message_callback& send)
{
    rtc::message_vector result;

    for (const auto& message : messages) {
        size_t offset = 0;

        while (offset < message->size()) {
            auto remaining_bytes = message->size() - offset;
            auto fragment_size = std::min(remaining_bytes, max_fragment_size_);
            auto fragment = std::make_shared<rtc::binary>();
            fragment->reserve(fragment_size + 1);

            if (offset == 0) {
                // Start fragment.
                fragment->emplace_back(std::byte{1});;
            } else if (offset + max_fragment_size_ < message->size()) {
                // Middle fragment.
                fragment->emplace_back(std::byte{2});;
            } else {
                // End fragment.
                fragment->emplace_back(std::byte{3});;
            }

            std::copy(message->begin() + offset,
                      message->begin() + offset + fragment_size,
                      std::back_inserter(*fragment));

            result.push_back(packetize(fragment, false));

            offset += fragment_size;
        }
    }

    messages.swap(result);
}

} // namespace vacon
