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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rtc/rtc.hpp>

#include "codecs.hpp"

namespace vacon {

std::optional<rtc::Description::Media*> DescriptionMediaByMid(rtc::Description& desc, std::string_view mid);

int DescriptionMediaPayloadTypeByFormat(rtc::Description::Media* media, std::string_view format);

VideoCodec DescriptionVideoCodec(rtc::Description::Media* desc);

void LogDescriptionVideo(rtc::Description& desc,
                         std::optional<std::string_view> extra = std::nullopt);

std::optional<VideoCodec> SelectBestVideoCodec(rtc::Description::Media* media,
                                               std::shared_ptr<std::vector<VideoCodec>> our_codecs);

constexpr std::string ToString(rtc::Description::Direction dir);

} // namespace vacon
