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

#include <chrono>
#include <cstdint>
#include <vector>

#include "../common.hpp"
#include "camera_frame.hpp"

namespace vacon {
namespace linux {

struct CameraParams {
    std::string device;
    std::string pixel_format;
    int width;
    int height;
    int frame_rate;
    uint32_t n_kernel_buffers = 4;
};

class Camera {
    public:
        static std::shared_ptr<Camera> Create(const CameraParams&);
        Camera(Camera&&) = default;
        ~Camera();
        bool Init();
        bool StartCapturing();

        CameraFrame* ReadFrame();

    private:
        Camera() = default;

        bool OpenCameraDevice();
        bool InitV4L2();
        bool InitKernelBuffers();

        CameraParams params_;

        int fd_ = -1;
        uint32_t fourcc_ = 0;
        unsigned stream_skip_ = 15;
        std::vector<CameraFrame> frames_;
        std::chrono::time_point<std::chrono::steady_clock> t_last_;
};

} // namespace linux
} // namespace vacon
