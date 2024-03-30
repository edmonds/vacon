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
#include <string>
#include <tuple>
#include <vector>

#include <mfx.h>

#include "codecs.hpp"

namespace vacon {
namespace linux {

typedef std::vector<std::tuple<const char *, uint32_t>> mfxConfigFilters;

mfxSession GetMfxSession();
bool SetMfxLoaderConfigFilters(mfxLoader loader, mfxConfigFilters filters);
bool SetMfxLoaderConfigFiltersCombined(mfxLoader loader, mfxConfigFilters filters);
const char* MfxStatusStringConstant(mfxStatus status);
std::string MfxStatusStr(mfxStatus status);

constexpr VideoCodec FromMfxCodecAndFormat(mfxU32 codec, mfxU32 fmt)
{
    if (codec == MFX_CODEC_AVC && fmt == MFX_FOURCC_NV12) {
        return VideoCodec::AVC_8_420;
    } else if (codec == MFX_CODEC_HEVC) {
        if (fmt == MFX_FOURCC_NV12) {
            return VideoCodec::HEVC_8_420;
        } else if (fmt == MFX_FOURCC_P010) {
            return VideoCodec::HEVC_10_420;
        }
    } else if (codec == MFX_CODEC_VP9) {
        if (fmt == MFX_FOURCC_NV12) {
            return VideoCodec::VP9_8_420;
        } else if (fmt == MFX_FOURCC_P010) {
            return VideoCodec::VP9_10_420;
        }
    } else if (codec == MFX_CODEC_AV1) {
        if (fmt == MFX_FOURCC_NV12) {
            return VideoCodec::AV1_8_420;
        } else if (fmt == MFX_FOURCC_P010) {
            return VideoCodec::AV1_10_420;
        }
    }
    return VideoCodec::UNKNOWN;
}

} // namespace linux
} // namespace vacon
