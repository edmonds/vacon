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

#include "camera_frame.hpp"

#include <cassert>
#include <cerrno>
#include <memory>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fmt/format.h>

#include "../common.hpp"
#include "camera.hpp"

namespace vacon {
namespace linux {

std::shared_ptr<CameraFrame>
CameraFrame::Create(const Camera* camera,
                    struct v4l2_buffer buf,
                    const void *data)
{
    assert(camera);
    assert(data);

    auto frame      = std::make_shared<CameraFrame>(CameraFrame {});

    frame->buf_     = buf;
    frame->data_    = data;
    frame->camera_  = camera;

    return frame;
}

CameraFrame::~CameraFrame()
{
    if (camera_
        && camera_->fd_ != -1
        && ioctl(camera_->fd_, VIDIOC_QBUF, &buf_) == -1)
    {
        PLOG_FATAL << fmt::format("ioctl(VIDIOC_QBUF) on fd {}, buffer {} failed: {} ({})",
                                  camera_->fd_, buf_.index, errno, strerror(errno));
    }
}

uint64_t CameraFrame::PtsMicros()
{
    return buf_.timestamp.tv_sec * 1'000'000 + buf_.timestamp.tv_usec;
}

struct v4l2_pix_format CameraFrame::Fmt()
{
    return camera_->fmt_;
}

} // namespace linux
} // namespace vacon
