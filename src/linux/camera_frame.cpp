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
#include <cstring>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <fmt/format.h>

#include "../common.hpp"

namespace vacon {
namespace linux {

bool CameraFrame::Map()
{
    assert(data_ == nullptr);

    data_ = mmap(NULL, buf_.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf_.m.offset);
    if (data_ == MAP_FAILED) {
        PLOG_FATAL << fmt::format("mmap() on fd {}, buffer {} failed: {} ({})",
                                  fd_, buf_.index, errno, strerror(errno));
        return false;
    }

    // Success.
    return true;
}

bool CameraFrame::Unmap()
{
    if (data_) {
        if (munmap(data_, buf_.length) == -1) {
            PLOG_FATAL << fmt::format("munmap() on fd {}, buffer {} failed: {} ({})",
                                      fd_, buf_.index, errno, strerror(errno));
            return false;
        }
        data_ = nullptr;
    }

    // Success.
    return true;
}

bool CameraFrame::AcquireFromKernel()
{
    if (ioctl(fd_, VIDIOC_DQBUF, &buf_) == -1) {
        PLOG_FATAL << fmt::format("ioctl(VIDIOC_DQBUF) on fd {}, buffer {} failed: {} ({})",
                                  fd_, buf_.index, errno, strerror(errno));
        return false;
    }

    // Success.
    return true;
}

bool CameraFrame::ReleaseToKernel()
{
    if (ioctl(fd_, VIDIOC_QBUF, &buf_) == -1) {
        PLOG_FATAL << fmt::format("ioctl(VIDIOC_QBUF) on fd {}, buffer {} failed: {} ({})",
                                  fd_, buf_.index, errno, strerror(errno));
        return false;
    }

    // Success.
    return true;
}

} // namespace linux
} // namespace vacon
