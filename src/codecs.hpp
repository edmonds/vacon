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

#include <string>
#include <string_view>

namespace vacon {

enum class VideoCodec {
    UNKNOWN,
    AV1_10_420,
    AV1_8_420,
    VP9_10_420,
    HEVC_10_420,
    VP9_8_420,
    HEVC_8_420,
    AVC_8_420,
};

constexpr VideoCodec FromString(std::string_view codec) {
    if (codec == "AV1_10_420")  return VideoCodec::AV1_10_420;
    if (codec == "AV1_8_420")   return VideoCodec::AV1_8_420;
    if (codec == "VP9_10_420")  return VideoCodec::VP9_10_420;
    if (codec == "HEVC_10_420") return VideoCodec::HEVC_10_420;
    if (codec == "VP9_8_420")   return VideoCodec::VP9_8_420;
    if (codec == "HEVC_8_420")  return VideoCodec::HEVC_8_420;
    if (codec == "AVC_8_420")   return VideoCodec::AVC_8_420;
    return VideoCodec::UNKNOWN;
}

constexpr std::string ToString(VideoCodec codec) {
    switch (codec) {
        case VideoCodec::AV1_10_420:    return "AV1_10_420";
        case VideoCodec::AV1_8_420:     return "AV1_8_420";
        case VideoCodec::VP9_10_420:    return "VP9_10_420";
        case VideoCodec::HEVC_10_420:   return "HEVC_10_420";
        case VideoCodec::VP9_8_420:     return "VP9_8_420";
        case VideoCodec::HEVC_8_420:    return "HEVC_8_420";
        case VideoCodec::AVC_8_420:     return "AVC_8_420";
        default: return "UNKNOWN-CODEC";
    }
}

} // namespace vacon
