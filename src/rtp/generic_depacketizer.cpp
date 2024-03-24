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

/**
 * Copyright (c) 2023-2024 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "rtp/generic_depacketizer.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <utility>

#include <rtc/rtc.hpp>
#include <plog/Log.h>

namespace vacon {

rtc::message_vector GenericRtpDepacketizer::ReassemblePackets(rtc::message_vector::iterator begin,
                                                              rtc::message_vector::iterator end,
                                                              uint32_t timestamp)
{
    rtc::message_vector out = {};
    auto buf = rtc::binary{};
    auto frame_info = std::make_shared<rtc::FrameInfo>(timestamp);
    auto frag_sequence_started = false;
    ssize_t last_sequence = -2;

    for (auto it = begin; it != end; ++it) {
        auto rtp = it->get();
        auto rtp_parsed = reinterpret_cast<const rtc::RtpHeader *>(rtp->data());
        auto rtp_header_size = rtp_parsed->getSize() + rtp_parsed->getExtensionHeaderSize();
        auto fragment_header = rtp->at(rtp_header_size);

        if (fragment_header == std::byte{1}) {
            // Start fragment.
            if (frag_sequence_started) {
                LOG_DEBUG << "Got start fragment header, but fragment sequence already started?";
                return out;
            }
            frag_sequence_started = true;
        } else if (fragment_header == std::byte{2}) {
            // Middle fragment.
            if (!frag_sequence_started) {
                // Start fragment wasn't seen.
                LOG_DEBUG << "Got middle fragment but fragment sequence not started, dropped fragment?";
                return out;
            }
            if (last_sequence + 1 != rtp_parsed->seqNumber()) {
                LOG_DEBUG << std::format("Gap in sequence number (last {}, current {}), dropped fragment?", last_sequence, rtp_parsed->seqNumber());
                return out;
            }
        } else if (fragment_header == std::byte{3}) {
            // End fragment.
            frag_sequence_started = false;
            if (last_sequence + 1 != rtp_parsed->seqNumber()) {
                LOG_DEBUG << std::format("Gap in sequence number (last {}, current {}), dropped fragment?", last_sequence, rtp_parsed->seqNumber());
                return out;
            }
        } else {
            // Unknown kind of packet.
            LOG_DEBUG << std::format("Got unknown fragment header value: {}",
                                     std::to_integer<uint8_t>(fragment_header));
            return out;
        }

        last_sequence = rtp_parsed->seqNumber();
        if (last_sequence == std::numeric_limits<uint16_t>::max()) [[unlikely]] {
            last_sequence = -1;
        }

        std::copy(rtp->begin() + rtp_header_size + sizeof(std::byte),
                  rtp->end(),
                  std::back_inserter(buf));
    }

    if (!buf.empty()) {
        out.emplace_back(make_message(buf.begin(), buf.end(),
                                      rtc::Message::Binary, 0, nullptr, frame_info));
    }

    return out;
}

void GenericRtpDepacketizer::incoming(rtc::message_vector& messages, const rtc::message_callback&)
{
    messages.erase(std::remove_if(messages.begin(), messages.end(),
                                  [&](rtc::message_ptr message) {
                                      if (message->type == rtc::Message::Control) {
                                          return false;
                                      }

                                      if (message->size() < sizeof(rtc::RtpHeader)) {
                                          LOG_VERBOSE << "RTP packet is too small, size="
                                                      << message->size();
                                          return true;
                                      }

                                      rtp_buffer_.push_back(std::move(message));
                                      return true;
                                  }),
                   messages.end());

    while (!rtp_buffer_.empty()) {
        uint32_t current_timestamp = 0;
        size_t packets_in_timestamp = 0;

        for (const auto& pkt : rtp_buffer_) {
            auto rtp = reinterpret_cast<const rtc::RtpHeader *>(pkt->data());

            if (current_timestamp == 0) {
                current_timestamp = rtp->timestamp();
            } else if (current_timestamp != rtp->timestamp()) {
                break;
            }
            
            ++packets_in_timestamp;
        }

        if (packets_in_timestamp == rtp_buffer_.size()) {
            break;
        }

        auto begin = rtp_buffer_.begin();
        auto end = rtp_buffer_.begin() + packets_in_timestamp;

        auto packets = ReassemblePackets(begin, end, current_timestamp);
        messages.insert(messages.end(), packets.begin(), packets.end());

        rtp_buffer_.erase(begin, end);
    }
}

} // namespace vacon
