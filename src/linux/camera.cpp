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

#include "camera.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>

#include <fmt/format.h>

#include "../common.hpp"

using namespace std::chrono_literals;

namespace vacon {
namespace linux {

void PrintV4L2RequestBuffers(const struct v4l2_requestbuffers *reqbuf)
{
    PLOG_VERBOSE << fmt::format("count = {}", reqbuf->count);
    PLOG_VERBOSE << fmt::format("type = {}", reqbuf->type);
    PLOG_VERBOSE << fmt::format("memory = {}", reqbuf->memory);
    PLOG_VERBOSE << fmt::format("capabilities = {:#010x}", reqbuf->capabilities);
    PLOG_VERBOSE << fmt::format("flags = {}", reqbuf->flags);
}

void PrintV4L2Capability(const struct v4l2_capability *cap)
{
    PLOG_VERBOSE << fmt::format("driver = '{}'", (const char*)&cap->driver[0]);
    PLOG_VERBOSE << fmt::format("card = '{}'", (const char*)&cap->card[0]);
    PLOG_VERBOSE << fmt::format("bus_info = '{}'", (const char*)&cap->bus_info[0]);
    PLOG_VERBOSE << fmt::format("version = {:#010x}", cap->version);
    PLOG_VERBOSE << fmt::format("capabilities = {:#010x}", cap->capabilities);
    PLOG_VERBOSE << fmt::format("device_caps = {:#010x}", cap->device_caps);
}

void PrintV4L2Format(const struct v4l2_format *fmt)
{
    if (fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        PLOG_VERBOSE << fmt::format("pix.width = {}", fmt->fmt.pix.width);
        PLOG_VERBOSE << fmt::format("pix.height = {}", fmt->fmt.pix.height);
        PLOG_VERBOSE << fmt::format("pix.pixelformat = {}", FourCcToString(fmt->fmt.pix.pixelformat));
    } else {
        PLOG_VERBOSE << fmt::format("type = {}", fmt->type);
    }
}

void PrintCameraParams(const CameraParams& params)
{
    PLOG_DEBUG << "device = "           << params.device;
    PLOG_DEBUG << "pixel format = "     << params.pixel_format;
    PLOG_DEBUG << "width = "            << params.width;
    PLOG_DEBUG << "height = "           << params.height;
    PLOG_DEBUG << "frame_rate = "       << params.frame_rate;
    PLOG_DEBUG << "n_kernel_buffers = " << params.n_kernel_buffers;
}

std::shared_ptr<Camera> Camera::Create(const CameraParams& params)
{
    auto cam = std::make_shared<Camera>(Camera {});
    cam->params_ = params;
    return cam;
}

bool Camera::Init()
{
    auto t_start = std::chrono::steady_clock::now();

    PrintCameraParams(params_);

    if (!OpenCameraDevice()) {
        PLOG_ERROR << "OpenCameraDevice() failed";
        return false;
    }

    if (!InitV4L2()) {
        PLOG_ERROR << "InitCameraDevice() failed";
        return false;
    }

    if (!InitKernelBuffers()) {
        PLOG_ERROR << "InitKernelBuffers() failed";
        return false;
    }

    t_last_ = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(t_last_ - t_start).count();
    PLOG_INFO << fmt::format("Initialized V4L2 device {} in {} ms", params_.device, millis);

    return true;
}

Camera::~Camera()
{
    if (fd_ != -1) {
        if (close(fd_) == 0) {
            PLOG_VERBOSE << fmt::format("close()'d V4L2 device {} (fd {})", params_.device, fd_);
        } else {
            PLOG_ERROR << fmt::format("close() failed on V4L2 device {} (fd {}): {} ({})",
                                      params_.device, fd_, errno, strerror(errno));
        }
    }
    for (auto& frame : frames_) {
        frame.Unmap();
    }
}

bool Camera::OpenCameraDevice()
{
    // Open the V4L2 device's character node.
    fd_ = open(params_.device.c_str(), O_RDWR, 0);
    if (fd_ == -1) {
        PLOG_ERROR << fmt::format("open() failed on V4L2 device {}: {} ({})",
                                  params_.device, errno, strerror(errno));
        return false;
    } else {
        PLOG_VERBOSE << fmt::format("open()'d V4L2 device {} (fd {})", params_.device, fd_);
    }

    // Success.
    return true;
}

bool Camera::InitV4L2()
{
    // VIDIOC_QUERYCAP
    struct v4l2_capability cap = {};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) == 0) {
        PrintV4L2Capability(&cap);
    } else {
        PLOG_ERROR << fmt::format("ioctl(VIDIOC_QUERYCAP) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        PLOG_ERROR << fmt::format("{} is not a video capture device", params_.device);
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        PLOG_ERROR << fmt::format("{} does not support streaming I/O", params_.device);
        return false;
    }

    // VIDIOC_CROPCAP
    struct v4l2_cropcap cropcap = {};
    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_CROPCAP, &cropcap) == 0) {
        // VIDIOC_S_CROP
        struct v4l2_crop crop = {};
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect;
        (void) ioctl(fd_, VIDIOC_S_CROP, &crop);
    }

    // VIDIOC_G_FMT
    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd_, VIDIOC_G_FMT, &fmt) == 0) {
        PLOG_VERBOSE << "Got V4L2 camera data format (VIDIO_G_FMT):";
        PrintV4L2Format(&fmt);
    } else {
        PLOG_ERROR << fmt::format("ioctl(VIDIOC_G_FMT) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // VIDIOC_S_FMT
    struct v4l2_format force_fmt = {};
    force_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    force_fmt.fmt.pix.width = params_.width;
    force_fmt.fmt.pix.height = params_.height;
    force_fmt.fmt.pix.pixelformat = fmt.fmt.pix.pixelformat;
    if (params_.pixel_format.length() == 4) {
        auto f = params_.pixel_format.c_str();
        force_fmt.fmt.pix.pixelformat = v4l2_fourcc(f[0], f[1], f[2], f[3]);
    }
    PLOG_VERBOSE << "Trying to force camera data format:";
    PrintV4L2Format(&force_fmt);
    if (ioctl(fd_, VIDIOC_S_FMT, &force_fmt) == 0) {
        PLOG_VERBOSE << "Driver set camera data format (VIDIO_S_FMT):";
        PrintV4L2Format(&force_fmt);
    } else {
        PLOG_ERROR << fmt::format("ioctl(VIDIOC_S_FMT) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }
    fourcc_ = force_fmt.fmt.pix.pixelformat;
    params_.width = force_fmt.fmt.pix.width;
    params_.height = force_fmt.fmt.pix.height;

    // VIDIOC_S_PARM
    struct v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe = {1, (uint32_t)params_.frame_rate};
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) == 0) {
        auto frame_time =
            double(parm.parm.capture.timeperframe.numerator) /
            double(parm.parm.capture.timeperframe.denominator);
        PLOG_VERBOSE << fmt::format("Set time per frame to {}/{} ({:f}) seconds",
                                    parm.parm.capture.timeperframe.numerator,
                                    parm.parm.capture.timeperframe.denominator,
                                    frame_time);
    } else {
        PLOG_ERROR << fmt::format("ioctl(VIDIO_S_PARM) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Success.
    return true;
}

bool Camera::InitKernelBuffers()
{
    // VIDIOC_REQBUFS
    struct v4l2_requestbuffers reqbuf = {};
    reqbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory = V4L2_MEMORY_MMAP;
    reqbuf.count = params_.n_kernel_buffers;
    if (ioctl(fd_, VIDIOC_REQBUFS, &reqbuf) == 0) {
        PLOG_VERBOSE << "VIDIOC_REQBUFS:";
        PrintV4L2RequestBuffers(&reqbuf);
    } else {
        PLOG_ERROR << fmt::format("ioctl(VIDIOC_REQBUFS) on fd {} failed: {} ({})",
                                  fd_, errno, strerror(errno));
        return false;
    }

    // VIDIOC_QUERYBUF
    for (unsigned i = 0; i < reqbuf.count; ++i) {
        struct v4l2_buffer buffer = {};
        buffer.type = reqbuf.type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buffer) == -1) {
            PLOG_ERROR << fmt::format("ioctl(VIDIOC_QUERYBUF) on fd {}, buffer {} failed: {} ({})",
                                      fd_, buffer.index, errno, strerror(errno));
            return false;
        }

        frames_.emplace_back(CameraFrame(buffer, fd_, fourcc_));
    }

    // VIDIOC_QBUF
    size_t n_bytes_mapped = 0;
    for (auto& frame : frames_) {
        if (!frame.Map()) {
            return false;
        }

        if (!frame.ReleaseToKernel()) {
            return false;
        }
        n_bytes_mapped += frame.buf_.length;
    }

    PLOG_DEBUG << fmt::format("Successfully mapped V4L2 memory, {} buffers, {:.2f} Mbytes",
                              frames_.size(), n_bytes_mapped / 1048576.0);

    // Success.
    return true;
}

bool Camera::StartCapturing()
{
    auto t_start = std::chrono::steady_clock::now();

    // VIDIOC_STREAMON
    enum v4l2_buf_type type(V4L2_BUF_TYPE_VIDEO_CAPTURE);
    if (ioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
        PLOG_ERROR << fmt::format("ioctl(VIDIOC_STREAMON) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Get the current file status flags of the capture file descriptor.
    auto flags = fcntl(fd_, F_GETFL);
    if (flags == -1) {
        PLOG_ERROR << fmt::format("fcntl(F_GETFL) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Set the capture file descriptor into nonblocking mode.
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
        PLOG_ERROR << fmt::format("fcntl(F_SETFL) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Skip the first few frames.
    for (;;) {
        struct pollfd pfd = {
            .fd = fd_,
            .events = POLLIN,
            .revents = 0,
        };

        auto status = poll(&pfd, 1, 2000 /* timeout in ms */);
        if (status == -1) {
            PLOG_ERROR << fmt::format("poll() on fd {} failed: {} ({})", fd_, errno, strerror(errno));
            return false;
        }

        if (pfd.revents & POLLIN) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            // VIDIOC_DQBUF
            if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
                PLOG_ERROR << fmt::format("ioctl(VIDIOC_DQBUF) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
                return false;
            }
            t_last_ = std::chrono::steady_clock::now();

            // VIDIOC_QBUF
            auto frame = &frames_.at(buf.index);
            frame->ReleaseToKernel();

            if (buf.sequence >= stream_skip_) {
                break;
            }
        } else {
            PLOG_ERROR << fmt::format("fd {} is ready but not for data, poll events {:#x}", fd_, pfd.revents);
            return false;
        }
    }

    // Put the capture file descriptor back into blocking mode.
    if (fcntl(fd_, F_SETFL, flags) == -1) {
        PLOG_ERROR << fmt::format("fcntl(F_SETFL) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    auto t_end = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    PLOG_INFO << fmt::format("Started capturing from V4L2 device {} in {} ms", params_.device, millis);

    // Success.
    return true;
}

CameraFrame* Camera::ReadFrame()
{
    struct v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    for (;;) {
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
            PLOG_ERROR << fmt::format("ioctl(VIDIOC_DQBUF) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
            return nullptr;
        }

        auto t_now = std::chrono::steady_clock::now();
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t_now - t_last_).count();
        t_last_ = t_now;
        PLOG_VERBOSE << fmt::format("Received frame on fd {}, buffer {}, sequence {}, delta {} us",
                                    fd_, buf.index, buf.sequence, micros);

        auto frame = &frames_.at(buf.index);
        frame->buf_ = buf;

        bool is_empty_frame = buf.bytesused == 0;
        bool is_error_frame = buf.flags & V4L2_BUF_FLAG_ERROR;

        if (is_empty_frame || is_error_frame) {
            PLOG_VERBOSE << fmt::format("Discarding frame: is_empty_frame {}, is_error_frame {}",
                                        is_empty_frame, is_error_frame);
            frame->ReleaseToKernel();
            continue;
        }

        return frame;
    }
}

} // namespace linux
} // namespace vacon
