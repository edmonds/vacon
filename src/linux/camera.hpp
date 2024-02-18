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
#include <span>
#include <utility>
#include <vector>

#include <linux/videodev2.h>

#include <SDL3/SDL.h>

namespace vacon {
namespace linux {

struct CameraParams {
    std::string device;
    std::string pixel_format;
    uint32_t width;
    uint32_t height;
    uint32_t frame_rate;

    uint32_t n_kernel_buffers = 8;
    uint32_t n_initial_stream_skip_frames = 15;
};

enum class ChromaFormat {
    Invalid,
    YUV420_8,
    YUV422_8,
};

struct CameraFormat {
    // The sort key.
    float           frame_rate = 0.0f;
    ChromaFormat    chroma_format = ChromaFormat::Invalid;
    uint32_t        width = 0;
    uint32_t        height = 0;

    // The parameters to pass to VIDIOC_S_FMT, VIDIOC_S_PARM.
    v4l2_format     fmt = {};
    v4l2_streamparm parm = {};
};

struct CameraBuffer {
    v4l2_buffer                 vbuf = {};
    v4l2_exportbuffer           expbuf = {};
    v4l2_pix_format             fmt = {};
    SDL_Texture*                texture = nullptr;
    std::span<const std::byte>  mmap = {};

    uint64_t PtsMicros() const {
        return vbuf.timestamp.tv_sec * 1'000'000 + vbuf.timestamp.tv_usec;
    }
};

class CameraBufferRef {
    public:
        static std::shared_ptr<CameraBufferRef> Create(CameraBuffer& buf, int v4l2_fd);
        CameraBufferRef(CameraBufferRef&&);
        ~CameraBufferRef();

        const CameraBuffer& buf_;

    private:
        CameraBufferRef(CameraBuffer& buf, int v4l2_fd)
            : buf_(buf), v4l2_fd_(v4l2_fd) {};

        int v4l2_fd_ = -1;
};

class Camera {
    public:
        static std::shared_ptr<Camera> Create(const CameraParams&);
        Camera(Camera&&) = default;
        ~Camera();
        bool Init();
        bool ExportBuffersToOpenGL(SDL_Renderer*);
        bool StartCapturing();
        std::shared_ptr<CameraBufferRef> NextFrame();

        struct v4l2_pix_format fmt_ = {};

    private:
        Camera() = default;
        Camera(const CameraParams& params)
            : params_(params) {};
        bool OpenDevice();
        bool InitV4L2();
        bool EnumerateFormats();
        bool InitBuffers();
        bool ExportBufferToVaapi(CameraBuffer&);
        bool ExportBufferToMfx(CameraBuffer&);

        CameraParams params_;

        int fd_ = -1;
        std::vector<CameraBuffer> bufs_ = {};
        std::chrono::time_point<std::chrono::steady_clock> t_last_;

        std::vector<CameraFormat> formats_ = {};
};

} // namespace linux
} // namespace vacon
