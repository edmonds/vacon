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
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <linux/videodev2.h>

#include "common.hpp"
#include "linux/camera_frame.hpp"

namespace vacon {
namespace linux {

struct CameraParams {
    std::string device;
    std::string pixel_format;
    int width;
    int height;
    int frame_rate;
    uint32_t n_kernel_buffers = 8;
    uint32_t n_initial_stream_skip_frames_ = 15;
};

class Camera {
    friend class CameraFrame;

    public:
        static std::shared_ptr<Camera> Create(const CameraParams&);
        Camera(Camera&&) = default;
        ~Camera();
        bool Init();
        bool StartCapturing();

        std::shared_ptr<CameraFrame> ReadFrame();

    private:
        Camera() = default;

        bool OpenCameraDevice();
        bool InitV4L2();
        bool InitKernelBuffers();

        CameraParams params_;

        int fd_ = -1;
        struct v4l2_pix_format fmt_ = {};
        std::chrono::time_point<std::chrono::steady_clock> t_last_;

        // Camera frame buffer data and length pairs. The pointer is mmap()'d
        // V4L2 memory.
        std::vector<std::pair<void*, size_t>> mmap_buffers_;
};

} // namespace linux
} // namespace vacon
