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

void FreeMfxSurface(mfxFrameSurface1 **surface)
{
    if (*surface) {
        if ((*surface)->Data.R) {
            PLOG_VERBOSE << fmt::format("Unmapping MFX surface @ {}", fmt::ptr(*surface));
            auto status = (*surface)->FrameInterface->Unmap(*surface);
            if (status != MFX_ERR_NONE) {
                PLOG_DEBUG << "surface->FrameInterface->Unmap() failed: " << status;
            }
        }
        PLOG_VERBOSE << fmt::format("Releasing MFX surface @ {}", fmt::ptr(*surface));
        auto status = (*surface)->FrameInterface->Release(*surface);
        if (status != MFX_ERR_NONE) {
            PLOG_DEBUG << "surface->FrameInterface->Release() failed: " << status;
        }
        *surface = nullptr;
    }
}

VideoFrame::~VideoFrame()
{
    //PLOG_VERBOSE << fmt::format("Destroying VideoFrame @ {}", fmt::ptr(this));

    if (bitstream.Data) {
        free(bitstream.Data);
        bitstream.Data = nullptr;
    }

    FreeMfxSurface(&surface);
}

const std::byte* VideoFrame::CompressedData()
{
    return reinterpret_cast<const std::byte*>(bitstream.Data + bitstream.DataOffset);
}

size_t VideoFrame::CompressedDataLength()
{
    return bitstream.DataLength;
}

bool VideoFrame::ImportCameraFrame(const CameraFrame& camera, const mfxFrameInfo& info)
{
    auto width = info.CropW;
    auto height = info.CropH;

    surface_ref.Info = info;

    switch (camera.fourcc_) {
    case V4L2_PIX_FMT_NV12: {
        size_t bytes_needed = width * height * 3 / 2;
        if (bytes_needed != (size_t)camera.buf_.bytesused) {
            PLOG_ERROR << fmt::format("Camera frame is {} bytes, but MFX surface needs {} bytes",
                                      camera.buf_.bytesused, bytes_needed);
            return false;
        }
        surface_ref.Data.Y = reinterpret_cast<mfxU8*>(camera.data_);
        surface_ref.Data.UV = surface_ref.Data.Y + width * height;
        surface_ref.Data.Pitch = width;
        break;
    }
    default:
        PLOG_ERROR << fmt::format("Unsupported camera frame FourCC {} ({:#010x})",
                                  FourCcToString(camera.fourcc_), camera.fourcc_);
        return false;
    }

    // Success.
    return true;
}

} // namespace linux
} // namespace vacon
