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

#include "rtc_utils.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <rtc/rtc.hpp>
#include <plog/Log.h>

#include "codecs.hpp"

namespace vacon {

std::optional<rtc::Description::Media*> DescriptionMediaByMid(rtc::Description& desc, std::string_view mid)
{
    for (unsigned int i = 0; i < desc.mediaCount(); ++i) {
        if (std::holds_alternative<rtc::Description::Media*>(desc.media(i))) {
            auto media = std::get<rtc::Description::Media*>(desc.media(i));
            if (media->mid() == mid) {
                return media;
            }
        }
    }
    return std::nullopt;
}

int DescriptionMediaPayloadTypeByFormat(rtc::Description::Media* media, std::string_view format)
{
    for (auto ptype : media->payloadTypes()) {
        if (media->rtpMap(ptype)->format == format) {
            return ptype;
        }
    }
    LOG_ERROR << std::format("Could not find format {} in payload types", format);
    return -1;
}

VideoCodec DescriptionVideoCodec(rtc::Description::Media* desc)
{
    auto payload_types = desc->payloadTypes();
    if (payload_types.size() != 1) {
        return VideoCodec::UNKNOWN;
    }
    return FromString(desc->rtpMap(payload_types.front())->format);
}

void LogDescriptionVideo(rtc::Description& desc, std::optional<std::string_view> extra)
{
    if (extra) {
        LOG_DEBUG << extra.value();
    }

    for (unsigned int i = 0; i < desc.mediaCount(); ++i) {
        if (std::holds_alternative<rtc::Description::Media*>(desc.media(i))) {
            auto media = std::get<rtc::Description::Media*>(desc.media(i));
            if (media->type() != "video") {
                continue;
            }
            auto payload_types = media->payloadTypes();
            for (auto payload_type : payload_types) {
                const auto rtp_map = media->rtpMap(payload_type);
                LOG_DEBUG <<
                    std::format("Video #{}, mid {}, payload type {}, format {}, direction {}",
                                i, media->mid(), payload_type, rtp_map->format,
                                ToString(media->direction()));
            }
        }
    }
}

std::optional<VideoCodec> SelectBestVideoCodec(rtc::Description::Media* media,
                                               std::shared_ptr<std::vector<VideoCodec>> our_codecs)
{
    std::vector<VideoCodec> their_codecs = {};
    for (auto ptype : media->payloadTypes()) {
        their_codecs.emplace_back(FromString(media->rtpMap(ptype)->format));
    }

    auto best = std::find_first_of(their_codecs.begin(), their_codecs.end(),
                                   our_codecs->begin(), our_codecs->end());
    if (best != their_codecs.end()) {
        for (auto ptype : media->payloadTypes()) {
            auto format = media->rtpMap(ptype)->format;
            VideoCodec codec = FromString(format);
            if (codec != *best) {
                media->removeFormat(format);
            }
        }
        return *best;
    }

    return std::nullopt;
}

constexpr std::string ToString(rtc::Description::Direction dir)
{
    if (dir == rtc::Description::Direction::SendRecv) return "SendRecv";
    if (dir == rtc::Description::Direction::SendOnly) return "SendOnly";
    if (dir == rtc::Description::Direction::RecvOnly) return "RecvOnly";
    if (dir == rtc::Description::Direction::Inactive) return "Inactive";
    if (dir == rtc::Description::Direction::Unknown)  return "Unknown";
    return "INVALID";
}

} // namespace vacon
