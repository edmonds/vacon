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
#include <memory>

#include <linux/videodev2.h>

#include "common.hpp"

namespace vacon {
namespace linux {

class Camera;

class CameraFrame {
    public:
        static std::shared_ptr<CameraFrame> Create(const Camera* camera,
                                                   struct v4l2_buffer buf,
                                                   const void *data);
        CameraFrame(CameraFrame&&) = default;
        ~CameraFrame();

        uint64_t PtsMicros();
        struct v4l2_pix_format Fmt();

        struct v4l2_buffer buf_ = {};
        const void *data_ = nullptr;

    private:
        CameraFrame() = default;
        const Camera* camera_ = nullptr;
};

} // namespace linux
} // namespace vacon
