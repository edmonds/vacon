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

#include <linux/videodev2.h>

#include "../common.hpp"

namespace vacon {
namespace linux {

class CameraFrame {
    public:
        CameraFrame(struct v4l2_buffer buf, int fd, uint32_t fourcc)
            : buf_(buf), fourcc_(fourcc), fd_(fd) {};
        CameraFrame(CameraFrame&&) = default;
        ~CameraFrame() = default;

        bool Map();
        bool Unmap();
        bool AcquireFromKernel();
        bool ReleaseToKernel();

        uint64_t pts() { return buf_.timestamp.tv_sec * 1'000'000 + buf_.timestamp.tv_usec; }

        struct v4l2_buffer buf_ = {};
        void *data_ = nullptr;
        uint32_t fourcc_ = 0;

    private:
        int fd_ = -1;
};

} // namespace linux
} // namespace vacon
