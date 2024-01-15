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

#include <cassert>
#include <cstddef>
#include <cstdint>

#include <mfx.h>

#include "../common.hpp"
#include "camera_frame.hpp"

namespace vacon {
namespace linux {

struct VideoFrame {
    uint64_t pts = 0;
    mfxBitstream bitstream = {};
    mfxFrameSurface1 *surface = nullptr;

    VideoFrame(uint32_t max_length = 262144)
    {
        bitstream.MaxLength = (mfxU32)max_length;
        bitstream.Data = (mfxU8*)calloc(bitstream.MaxLength, 1);
        assert(bitstream.Data);
    }

    VideoFrame(VideoFrame&& src)
    {
        bitstream           = src.bitstream;
        surface             = src.surface;

        src.bitstream       = {};
        src.surface         = nullptr;
    }

    ~VideoFrame();

    const std::byte* CompressedData();
    size_t CompressedDataLength();
};

} // namespace linux
} // namespace vacon
