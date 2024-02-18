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
#include <thread>
#include <utility>
#include <vector>

#include <linux/videodev2.h>

#include <SDL3/SDL.h>

#include "linux/typedefs.hpp"
#include "util.hpp"

namespace vacon {
namespace linux {

struct CameraParams {
    std::string device;
    std::shared_ptr<CameraBufferQueue> encoder_queue = nullptr;
    std::shared_ptr<CameraBufferQueue> preview_queue = nullptr;

    uint32_t n_kernel_buffers = 8;
    uint32_t n_initial_stream_skip_frames = 15;
};

enum class ChromaFormat {
    Invalid,
    YUV420_8,
    YUV422_8,
};

struct CameraFormat {
    // The V4L2 parameters to pass to VIDIOC_S_FMT and VIDIOC_S_PARM when
    // initializing a V4L2 device.
    v4l2_format     fmt = {};
    v4l2_streamparm parm = {};

    // Helper functions to grovel around in the V4L2 parameters.
    std::string FourCcStr() const { return util::FourCcToString(FourCc()); }
    uint32_t FourCc() const { return fmt.fmt.pix.pixelformat; }
    uint32_t Width() const { return fmt.fmt.pix.width; }
    uint32_t Height() const { return fmt.fmt.pix.height; }
    uint32_t FrameTimeN() const { return parm.parm.capture.timeperframe.numerator; }
    uint32_t FrameTimeD() const { return parm.parm.capture.timeperframe.denominator; }
    uint32_t FrameRateN() const { return parm.parm.capture.timeperframe.denominator; }
    uint32_t FrameRateD() const { return parm.parm.capture.timeperframe.numerator; }
    float FrameTime() const { return (float)FrameTimeN() / (float)FrameTimeD(); }
    float FrameRate() const { return (float)FrameRateN() / (float)FrameRateD(); }
    ChromaFormat Chroma() const
    {
        switch (FourCc()) {
        case V4L2_PIX_FMT_YUYV: [[fallthrough]];
        case V4L2_PIX_FMT_UYVY: return ChromaFormat::YUV422_8;
        case V4L2_PIX_FMT_NV12: return ChromaFormat::YUV420_8;
        default: return ChromaFormat::Invalid;
        }
    }
    std::string Str() const { return std::format("{}x{}@{} {}", Width(), Height(), FrameRate(), FourCcStr()); }
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
        static std::unique_ptr<Camera> Create(const CameraParams&);
        Camera(Camera&&) = default;
        ~Camera();
        bool Init();
        bool ExportBuffersToOpenGL(SDL_Renderer*);
        CameraFormat GetCameraFormat();

    private:
        Camera() = default;
        Camera(const CameraParams& params)
            : params_(params) {};
        void RunCamera(std::stop_token);
        bool InitCamera();

        bool OpenDevice();
        bool EnumerateFormats();
        bool InitV4L2();
        bool InitBuffers();
        bool StartCapturing();

        std::shared_ptr<CameraBufferRef> NextFrame();

        CameraParams                params_ = {};
        CameraFormat                format_ = {};
        int                         fd_ = -1;
        std::jthread                thread_ = {};
        std::vector<CameraBuffer>   bufs_ = {};
        std::vector<CameraFormat>   formats_ = {};
        v4l2_pix_format             pixfmt_ = {};

        std::chrono::time_point<std::chrono::steady_clock>
                                    t_last_ = {};
};

} // namespace linux
} // namespace vacon
