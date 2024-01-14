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

VideoFrame::~VideoFrame()
{
    //PLOG_VERBOSE << fmt::format("Destroying VideoFrame @ {}", fmt::ptr(this));

    if (bitstream.Data) {
        free(bitstream.Data);
        bitstream.Data = nullptr;
    }

    if (surface) {
        if (surface->Data.R) {
            //PLOG_VERBOSE << fmt::format("Unmapping MFX surface @ {}", fmt::ptr(surface));
            auto status = surface->FrameInterface->Unmap(surface);
            if (status != MFX_ERR_NONE) {
                PLOG_VERBOSE << "surface->FrameInterface->Unmap() returned: " << status;
            }
        }
        //PLOG_VERBOSE << fmt::format("Releasing MFX surface @ {}", fmt::ptr(surface));
        auto status = surface->FrameInterface->Release(surface);
        if (status != MFX_ERR_NONE) {
            PLOG_VERBOSE << "surface->FrameInterface->Release() returned: " << status;
        }
        surface = nullptr;
    }
}

const std::byte* VideoFrame::CompressedData()
{
    return reinterpret_cast<const std::byte*>(bitstream.Data + bitstream.DataOffset);
}

size_t VideoFrame::CompressedDataLength()
{
    return bitstream.DataLength;
}

bool VideoFrame::CopyCameraFrameToSurface(const CameraFrame& camera)
{
    auto width = surface->Info.CropW;
    auto height = surface->Info.CropH;

    switch (camera.fourcc_) {
    case V4L2_PIX_FMT_NV12: {
        // NV12 is a semi-planar 4:2:0 format that has one luminance plane (Y)
        // and one plane for the two chrominance components (UV). V4L2 returns
        // NV12 frames packed into a single buffer, but libvpl needs the frame
        // data split into Y and UV planes.
        size_t bytes_needed = width * height * 3 / 2;
        if (bytes_needed != (size_t)camera.buf_.bytesused) {
            PLOG_DEBUG << fmt::format("Camera frame is {} bytes, but encoder surface needs {} bytes",
                                      camera.buf_.bytesused, bytes_needed);
            return false;
        }

        // Copy the Y plane, (width * height) bytes.
        memcpy(surface->Data.Y, camera.data_, width * height);

        // Copy the UV plane, (width * height / 2) bytes.
        memcpy(surface->Data.UV,
               (unsigned char*)camera.data_ + width * height,
               width * height / 2);

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
