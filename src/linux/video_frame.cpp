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

#include "video_frame.hpp"

#include <fmt/format.h>
#include <mfx.h>

#include "../common.hpp"
#include "camera_frame.hpp"
#include "video_frame.hpp"

namespace vacon {
namespace linux {

void VideoFrame::FreeMfxSurface()
{
    if (surface) {
        if (surface->Data.R) {
            //PLOG_VERBOSE << fmt::format("Unmapping MFX surface @ {}", fmt::ptr(surface));
            auto status = surface->FrameInterface->Unmap(surface);
            if (status != MFX_ERR_NONE) {
                PLOG_DEBUG << "surface->FrameInterface->Unmap() failed: " << status;
            }
        }
        //PLOG_VERBOSE << fmt::format("Releasing MFX surface @ {}", fmt::ptr(surface));
        auto status = surface->FrameInterface->Release(surface);
        if (status != MFX_ERR_NONE) {
            PLOG_DEBUG << "surface->FrameInterface->Release() failed: " << status;
        }
        surface = nullptr;
    }
}

VideoFrame::~VideoFrame()
{
    //PLOG_VERBOSE << fmt::format("Destroying VideoFrame @ {}", fmt::ptr(this));

    if (bitstream.Data) {
        free(bitstream.Data);
        bitstream.Data = nullptr;
    }

    FreeMfxSurface();
}

const std::byte* VideoFrame::CompressedData()
{
    return reinterpret_cast<const std::byte*>(bitstream.Data + bitstream.DataOffset);
}

size_t VideoFrame::CompressedDataLength()
{
    return bitstream.DataLength;
}

} // namespace linux
} // namespace vacon
