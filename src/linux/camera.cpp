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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <thread>
#include <vector>

#include <linux/version.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_egl.h>
#include <SDL3/SDL_opengles2.h>
#include <plog/Log.h>

#include "event.hpp"
#include "util.hpp"

using namespace std::chrono_literals;

namespace vacon {
namespace linux {

std::atomic_size_t n_frames_camera_success          = 0;
std::atomic_size_t n_frames_camera_missed           = 0;
std::atomic_size_t n_frames_camera_overflow_encoder = 0;
std::atomic_size_t n_frames_camera_overflow_preview = 0;

static void LogV4L2RequestBuffers(const struct v4l2_requestbuffers *reqbuf)
{
    LOG_VERBOSE << std::format("count = {}", reqbuf->count);
    LOG_VERBOSE << std::format("type = {}", reqbuf->type);
    LOG_VERBOSE << std::format("memory = {}", reqbuf->memory);
    LOG_VERBOSE << std::format("capabilities = {:#010x}", reqbuf->capabilities);
#if LINUX_VERSION_MAJOR >= 6
    LOG_VERBOSE << std::format("flags = {}", reqbuf->flags);
#endif
}

static void LogV4L2Capability(const struct v4l2_capability *cap)
{
    LOG_VERBOSE << std::format("driver = '{}'", (const char*)&cap->driver[0]);
    LOG_VERBOSE << std::format("card = '{}'", (const char*)&cap->card[0]);
    LOG_VERBOSE << std::format("bus_info = '{}'", (const char*)&cap->bus_info[0]);
    LOG_VERBOSE << std::format("version = {:#010x}", cap->version);
    LOG_VERBOSE << std::format("capabilities = {:#010x}", cap->capabilities);
    LOG_VERBOSE << std::format("device_caps = {:#010x}", cap->device_caps);
}

static void LogV4L2Format(const struct v4l2_format *fmt)
{
    if (fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
        LOG_VERBOSE << "pix.width = "       << fmt->fmt.pix.width;
        LOG_VERBOSE << "pix.height = "      << fmt->fmt.pix.height;
        LOG_VERBOSE << "pix.pixelformat = " << util::FourCcToString(fmt->fmt.pix.pixelformat);
    } else {
        LOG_VERBOSE << "type = {}"          << fmt->type;
    }
}

std::unique_ptr<Camera> Camera::Create(const CameraParams& params)
{
    return std::make_unique<Camera>(Camera(params));
}

Camera::~Camera()
{
    RequestStop();
    Join();

    for (auto& buf : bufs_) {
        // Destroy the OpenGL texture.
        if (buf.texture) {
            LOG_VERBOSE << std::format("Destroying SDL_Texture @ {}",
                                       reinterpret_cast<void*>(buf.texture));
            SDL_ClearError();
            SDL_DestroyTexture(buf.texture);
            if (auto err = std::string(SDL_GetError()); err != "") {
                LOG_ERROR << std::format("SDL_DestroyTexture() failed: {}", err);
            }
            buf.texture = nullptr;
        }

        // Unmap the V4L2 frame buffer.
        if (buf.mmap.data()) {
            const void *ptr = buf.mmap.data();
            size_t len = buf.mmap.size_bytes();
            LOG_VERBOSE << std::format("Unmapping V4L2 buffer data @ {}, length {}", ptr, len);
            if (munmap(const_cast<void*>(ptr), len) == -1) {
                LOG_ERROR << std::format("munmap() data @ {}, length {} failed: {} ({})",
                                         ptr, len, errno, strerror(errno));
            }
            buf.mmap = {};
        }

        // Close the V4L2 dmabuf fd.
        if (buf.expbuf.fd != -1) {
            LOG_VERBOSE << std::format("Closing V4L2 dmabuf fd {}", buf.expbuf.fd);
            if (close(buf.expbuf.fd) != 0) {
                LOG_ERROR << std::format("close() failed on V4L2 dmabuf fd {}: {} ({})",
                                         buf.expbuf.fd, errno, strerror(errno));
            }
            buf.expbuf.fd = -1;
        }
    }

    // Close the V4L2 device.
    if (fd_ != -1) {
        LOG_VERBOSE << std::format("Closing V4L2 device {} (fd {})", params_.device, fd_);
        if (close(fd_) != 0) {
            LOG_ERROR << std::format("close() failed on V4L2 device {} (fd {}): {} ({})",
                                     params_.device, fd_, errno, strerror(errno));
        }
        fd_ = -1;
    }
}

void Camera::StartThread()
{
    thread_ = std::jthread([&](std::stop_token st) { RunCamera(st); });
}

void Camera::RequestStop()
{
    if (thread_.joinable()) {
        LOG_DEBUG << "Requesting stop of camera capture thread ID " << thread_.get_id();
        thread_.request_stop();
    }
}

void Camera::Join()
{
    if (thread_.joinable()) {
        LOG_DEBUG << "Joining camera capture thread ID " << thread_.get_id();
        thread_.join();
        thread_ = {};
    }
}

void Camera::RunCamera(std::stop_token st)
{
    LOG_DEBUG << "Starting camera capture thread ID " << std::this_thread::get_id();
    util::SetThreadName("VCameraCapture");

    PushEvent(Event::CameraStarting);
    if (!InitCamera()) {
        LOG_ERROR << "InitCamera() failed!";
        PushEvent(Event::CameraFailed);
        return;
    }
    PushEvent(Event::CameraStarted);

    uint32_t last_sequence = 0;
    while (!st.stop_requested()) {
        // Get the next V4L2 frame from the camera.
        auto cref = NextFrame();
        if (!cref) {
            continue;
        }
        n_frames_camera_success.fetch_add(1, std::memory_order_relaxed);

        // Check if any frames have been dropped.
        uint32_t sequence = cref->buf_.vbuf.sequence;
        if (last_sequence > 0 && (sequence != last_sequence + 1)) {
            LOG_DEBUG << std::format("Gap in camera frame sequence, current sequence {}, last sequence {}",
                                     sequence, last_sequence);
            n_frames_camera_missed.fetch_add(sequence - last_sequence, std::memory_order_relaxed);
        }
        last_sequence = sequence;

        // Enqueue the camera frame onto the encoder queue.
        if (params_.encoder_queue && !params_.encoder_queue->try_enqueue(cref)) {
            LOG_VERBOSE << "Failed to enqueue frame onto encoder queue, discarding!";
            n_frames_camera_overflow_encoder.fetch_add(1, std::memory_order_relaxed);
        }

        // Enqueue the camera frame onto the preview queue.
        if (params_.preview_queue && !params_.preview_queue->try_enqueue(cref)) {
            LOG_VERBOSE << "Failed to enqueue frame onto preview queue, discarding!";
            n_frames_camera_overflow_preview.fetch_add(1, std::memory_order_relaxed);
        }
    }

    LOG_DEBUG << "Stopping camera capture thread ID " << std::this_thread::get_id();
}

bool Camera::InitCamera()
{
    auto t_start = std::chrono::steady_clock::now();
    LOG_INFO << std::format("Initializing V4L2 device {}", params_.device);

    if (!OpenDevice()) {
        LOG_ERROR << "OpenDevice() failed";
        return false;
    }

    if (!EnumerateFormats()) {
        LOG_ERROR << "EnumerateFormats() failed";
        return false;
    }

    if (!InitV4L2()) {
        LOG_ERROR << "InitV4L2() failed";
        format_ = {};
        return false;
    }

    if (!InitBuffers()) {
        LOG_ERROR << "InitBuffers() failed";
        return false;
    }

    if (!StartCapturing()) {
        LOG_ERROR << "StartCapturing() failed";
        return false;
    }

    t_last_ = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(t_last_ - t_start).count();
    LOG_INFO << std::format("Initialized V4L2 device {} with format {} in {} ms",
                            params_.device, std::string(format_), millis);

    return true;
}

CameraFormat Camera::GetCameraFormat()
{
    return format_;
}

bool Camera::OpenDevice()
{
    // Open the V4L2 device's character node.
    fd_ = open(params_.device.c_str(), O_RDWR, 0);
    if (fd_ == -1) {
        LOG_ERROR << std::format("open() failed on V4L2 device {}: {} ({})",
                                 params_.device, errno, strerror(errno));
        return false;
    } else {
        LOG_VERBOSE << std::format("Opened V4L2 device {} (fd {})", params_.device, fd_);
    }

    // Success.
    return true;
}

bool Camera::EnumerateFormats()
{
    const uint32_t min_frame_height = 720;
    const uint32_t max_frame_height = 1080;
    const float min_frame_rate = 30.0f;
    const float max_frame_rate = 60.0f;

    const std::vector<uint32_t> pixelformat_allowlist = {
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_UYVY,
        V4L2_PIX_FMT_NV12,
    };

    // VIDIOC_ENUM_FMT
    struct v4l2_fmtdesc fmtdesc = {};
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (ioctl(fd_, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        ++fmtdesc.index;
        LOG_VERBOSE << std::format("fmtdesc: pixel format = {}, description = '{}'",
                                   util::FourCcToString(fmtdesc.pixelformat),
                                   (const char *)&fmtdesc.description[0]);

        // Check if this pixel format is on the allowlist.
        if (std::find(pixelformat_allowlist.begin(),
                      pixelformat_allowlist.end(),
                      fmtdesc.pixelformat) == pixelformat_allowlist.end())
        {
            LOG_VERBOSE << std::format("Ignoring pixel format '{}' not on allowlist",
                                       util::FourCcToString(fmtdesc.pixelformat));
            continue;
        }

        // VIDIOC_ENUM_FRAMESIZES
        struct v4l2_frmsizeenum frmsize = {};
        frmsize.pixel_format = fmtdesc.pixelformat;
        while (ioctl(fd_, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            ++frmsize.index;

            // Check frame size type.
            if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                LOG_VERBOSE << std::format("Ignoring v4l2_frmsizeenum::type != V4L2_FRMSIZE_TYPE_DISCRETE");
                continue;
            }

            LOG_VERBOSE << std::format("frmsize: width = {}, height = {}", frmsize.discrete.width, frmsize.discrete.height);

            // Check if the frame height is allowed.
            if (frmsize.discrete.height < min_frame_height ||
                frmsize.discrete.height > max_frame_height)
            {
                LOG_VERBOSE << std::format("Ignoring v4l2_frmsize_discrete::height {} out of bounds [{}, {}]",
                                           frmsize.discrete.height, min_frame_height, max_frame_height);
                continue;
            }

            // VIDIOC_ENUM_FRAMEINTERVALS
            struct v4l2_frmivalenum frmival = {};
            frmival.pixel_format = frmsize.pixel_format;
            frmival.width = frmsize.discrete.width;
            frmival.height = frmsize.discrete.height;
            while (ioctl(fd_, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
                ++frmival.index;

                // Check frame interval type.
                if (frmival.type != V4L2_FRMIVAL_TYPE_DISCRETE) {
                    LOG_VERBOSE << std::format("Ignoring v4l2_frmivalenum::type != V4L2_FRMIVAL_TYPE_DISCRETE");
                    continue;
                }

                // Construct the CameraFormat and its members.
                struct v4l2_format fmt          = {};
                fmt.type                        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                fmt.fmt.pix.width               = frmsize.discrete.width;
                fmt.fmt.pix.height              = frmsize.discrete.height;
                fmt.fmt.pix.pixelformat         = fmtdesc.pixelformat;

                struct v4l2_streamparm parm     = {};
                parm.type                       = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                parm.parm.capture.timeperframe  = frmival.discrete;

                auto format = CameraFormat {
                    .fmt    = fmt,
                    .parm   = parm,
                };

                LOG_VERBOSE << std::format("frmival: frame rate = {}", format.FrameRate());

                // Check if the frame rate is allowed.
                if (format.FrameRate() < min_frame_rate || format.FrameRate() > max_frame_rate) {
                    LOG_VERBOSE << std::format("Ignoring frame rate {} out of bounds [{}, {}]",
                                               format.FrameRate(), min_frame_rate, max_frame_rate);
                    continue;
                }

                // Add the CameraFormat to the list of approved formats.
                formats_.push_back(format);

            } // VIDIOC_ENUM_FRAMEINTERVALS
        } // VIDIOC_ENUM_FRAMESIZES
    } // VIDIOC_ENUM_FMT

    // Sort the camera formats from most preferred to least preferred.
    //
    // Frame rate is most important:
    // - 60 fps is better than 30 fps
    //
    // Then lines of resolution (but not more important than frame rate):
    // - 1080p60 is better than 720p60
    // - 720p60 is better than 1080p30
    //
    // Then frame width:
    // - 1280x720@60 is better than 960x720@60
    //
    // Then chroma format:
    // - 4:2:2 1920x1080@60 is better than 4:2:0 1920x1080@60
    // - 4:2:0 1920x1080@60 is better than 4:2:2 1280x720@60
    //
    std::sort(formats_.begin(), formats_.end(),
              [](const CameraFormat& a, const CameraFormat& b) { return a.Chroma() > b.Chroma(); });
    std::sort(formats_.begin(), formats_.end(),
              [](const CameraFormat& a, const CameraFormat& b) { return a.Width() > b.Width(); });
    std::sort(formats_.begin(), formats_.end(),
              [](const CameraFormat& a, const CameraFormat& b) { return a.Height() > b.Height(); });
    std::sort(formats_.begin(), formats_.end(),
              [](const CameraFormat& a, const CameraFormat& b) { return a.FrameRate() > b.FrameRate(); });

    for (auto& format : formats_) {
        LOG_DEBUG << std::format("Usable camera format: {}", std::string(format));
    }

    if (formats_.empty()) {
        LOG_ERROR << "No usable camera formats found!";
        return false;
    }

    // Success.
    return true;
}

bool Camera::InitV4L2()
{
    if (formats_.empty()) {
        LOG_ERROR << "No usable camera formats found!";
        return false;
    }
    format_ = formats_.front();

    // VIDIOC_QUERYCAP
    struct v4l2_capability cap = {};
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) == 0) {
        LogV4L2Capability(&cap);
    } else {
        LOG_ERROR << std::format("ioctl(VIDIOC_QUERYCAP) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        LOG_ERROR << std::format("{} is not a video capture device", params_.device);
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        LOG_ERROR << std::format("{} does not support streaming I/O", params_.device);
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
        LOG_VERBOSE << "Got V4L2 camera data format (VIDIO_G_FMT):";
        LogV4L2Format(&fmt);
    } else {
        LOG_ERROR << std::format("ioctl(VIDIOC_G_FMT) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // VIDIOC_S_FMT
    auto force_fmt = format_.fmt;
    LOG_VERBOSE << "Trying to force camera data format:";
    LogV4L2Format(&force_fmt);
    if (ioctl(fd_, VIDIOC_S_FMT, &force_fmt) == 0) {
        LOG_VERBOSE << "Driver set camera data format (VIDIO_S_FMT):";
        LogV4L2Format(&force_fmt);
    } else {
        LOG_ERROR << std::format("ioctl(VIDIOC_S_FMT) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // VIDIOC_S_PARM
    auto force_parm = format_.parm;
    if (ioctl(fd_, VIDIOC_S_PARM, &force_parm) != 0) {
        LOG_ERROR << std::format("ioctl(VIDIO_S_PARM) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Check the actual parameters selected by the driver. Since the parameters
    // sent to the driver with VIDIOC_S_FMT were originally enumerated by the
    // driver, they ought to be supported, so if they differ something has gone
    // horribly wrong.
    auto actual = CameraFormat {
        .fmt = force_fmt,
        .parm = force_parm,
    };
    if (actual.Width() != format_.Width() ||
        actual.Height() != format_.Height() ||
        actual.FourCc() != format_.FourCc() ||
        actual.FrameRate() != format_.FrameRate())
    {
        LOG_ERROR << std::format("Unable to set capture parameters! Tried to set {}, but driver used {}!",
                                 std::string(format_), std::string(actual));
        return false;
    }

    // Save the additional capture parameters returned by VIDIOC_S_FMT, e.g.
    // pixel pitch.
    pixfmt_ = force_fmt.fmt.pix;

    // Success.
    return true;
}

bool Camera::InitBuffers()
{
    // VIDIOC_REQBUFS
    struct v4l2_requestbuffers reqbuf   = {};
    reqbuf.type                         = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    reqbuf.memory                       = V4L2_MEMORY_MMAP;
    reqbuf.count                        = params_.n_kernel_buffers;

    if (ioctl(fd_, VIDIOC_REQBUFS, &reqbuf) == 0) {
        LOG_VERBOSE << "VIDIOC_REQBUFS:";
        LogV4L2RequestBuffers(&reqbuf);
    } else {
        LOG_ERROR << std::format("ioctl(VIDIOC_REQBUFS) on fd {} failed: {} ({})",
                                  fd_, errno, strerror(errno));
        return false;
    }

    for (unsigned index = 0; index < reqbuf.count; ++index) {
        // VIDIOC_QUERYBUF
        struct v4l2_buffer buf  = {};
        buf.type                = reqbuf.type;
        buf.memory              = V4L2_MEMORY_MMAP;
        buf.index               = index;

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) == -1) {
            LOG_ERROR << std::format("ioctl(VIDIOC_QUERYBUF) on fd {}, buffer {} failed: {} ({})",
                                      fd_, index, errno, strerror(errno));
            return false;
        }

        // Map the kernel buffer for this V4L2 buffer index into userspace
        // memory. According to the V4L2 documentation, the `prot` argument
        // should be set to PROT_READ | PROT_WRITE "regardless of the device
        // type and the direction of data exchange".
        auto data = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, buf.m.offset);
        if (data == MAP_FAILED) {
            LOG_ERROR << std::format("mmap() on fd {}, buffer {} failed: {} ({})",
                                     fd_, index, errno, strerror(errno));
            return false;
        }
        LOG_DEBUG << std::format("Mapped V4L2 buffer data @ {}, length {}", data, buf.length);

        // VIDIOC_EXPBUF
        struct v4l2_exportbuffer expbuf = {};
        expbuf.type                     = reqbuf.type;
        expbuf.flags                    = O_RDONLY | O_CLOEXEC;
        expbuf.index                    = index;

        if (ioctl(fd_, VIDIOC_EXPBUF, &expbuf) == -1) {
            LOG_ERROR << std::format("ioctl(VIDIOC_EXPBUF) on fd {}, buffer {} failed: {} ({})",
                                      fd_, index, errno, strerror(errno));
            return false;
        }
        LOG_VERBOSE << std::format("ioctl(VIDIOC_EXPBUF) on fd {}, buffer {} returned dmabuf fd {}",
                                    fd_, index, expbuf.fd);

        // VIDIOC_QBUF
        if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
            LOG_ERROR << std::format("ioctl(VIDIOC_QBUF) on fd {}, buffer {} failed: {} ({})",
                                      fd_, index, errno, strerror(errno));
            return false;
        }

        bufs_.emplace_back(CameraBuffer {
            .vbuf   = buf,
            .expbuf = expbuf,
            .fmt    = pixfmt_,
            .mmap   = std::span<const std::byte>(static_cast<const std::byte*>(data),
                                                 static_cast<size_t>(buf.length)),
        });
    }

    LOG_DEBUG << std::format("Exported {} V4L2 dmabuf file descriptors", bufs_.size());

    // Success.
    return true;
}

bool Camera::ExportBuffersToOpenGL(SDL_Renderer* sdl_renderer)
{
    switch (pixfmt_.pixelformat) {
    case V4L2_PIX_FMT_NV12: [[fallthrough]];
    case V4L2_PIX_FMT_UYVY: [[fallthrough]];
    case V4L2_PIX_FMT_YUYV:
        break;
    default:
        LOG_ERROR << std::format("Unhandled V4L2 pixel format {} ({:#010x})",
                                 util::FourCcToString(pixfmt_.pixelformat),
                                 pixfmt_.pixelformat);
        return false;
    }

    // Get the current EGL display.
    auto egl_display = eglGetCurrentDisplay();
    if (egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR << std::format("eglGetCurrentDisplay() failed with error code {:#010x}", eglGetError());
        return false;
    }

    for (auto& buf : bufs_) {
        // Construct the attribute list needed to create an EGLImage using the
        // `EGL_EXT_image_dma_buf_import` extension. These attributes are
        // sufficient for single plane pixel formats like YUYV.
        std::vector<EGLAttrib> attrs = {
            EGL_WIDTH,                      static_cast<EGLint>(pixfmt_.width),
            EGL_HEIGHT,                     static_cast<EGLint>(pixfmt_.height),
            EGL_LINUX_DRM_FOURCC_EXT,       static_cast<EGLint>(pixfmt_.pixelformat),
            EGL_DMA_BUF_PLANE0_PITCH_EXT,   static_cast<EGLint>(pixfmt_.bytesperline),
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,  0,
            EGL_DMA_BUF_PLANE0_FD_EXT,      buf.expbuf.fd,
        };

        if (pixfmt_.pixelformat == V4L2_PIX_FMT_NV12) {
            // NV12 is a "semi-planar" format and needs additional attributes
            // specifying the UV plane.
            std::vector<EGLAttrib> more_attrs = {
                EGL_DMA_BUF_PLANE1_PITCH_EXT,   static_cast<EGLint>(pixfmt_.bytesperline),
                EGL_DMA_BUF_PLANE1_OFFSET_EXT,  static_cast<EGLint>(pixfmt_.bytesperline * pixfmt_.height),
                EGL_DMA_BUF_PLANE1_FD_EXT,      buf.expbuf.fd,
            };
            attrs.insert(attrs.end(), more_attrs.begin(), more_attrs.end());
        }

        // Sentinel value at the end of the attribute list.
        attrs.emplace_back(EGL_NONE);

        // Create the EGLImage from the DMABUF.
        auto egl_image = eglCreateImage(egl_display,
                                        EGL_NO_CONTEXT,
                                        EGL_LINUX_DMA_BUF_EXT,
                                        nullptr,
                                        &attrs[0]);
        if (egl_image == EGL_NO_IMAGE) {
            LOG_ERROR << std::format("eglCreateImage() failed with error code {:#010x}", eglGetError());
            return false;
        }

        // Create the corresponding SDL_Texture for the EGLImage.
        buf.texture =
            SDL_CreateTexture(sdl_renderer,
                              SDL_PIXELFORMAT_EXTERNAL_OES,
                              SDL_TEXTUREACCESS_STATIC,
                              pixfmt_.width,
                              pixfmt_.height);
        if (!buf.texture) {
            LOG_ERROR << "SDL_CreateTexture() failed: " << SDL_GetError();
            return false;
        }
        SDL_SetTextureBlendMode(buf.texture, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(buf.texture, SDL_SCALEMODE_BEST);

        // Get the texture properties.
        auto texture_props = SDL_GetTextureProperties(buf.texture);
        if (texture_props == 0) {
            LOG_ERROR << "SDL_GetTextureProperties() failed: " << SDL_GetError();
            return false;
        }

        // Get the GL texture number of the texture.
        auto texture_id = static_cast<GLuint>
            (SDL_GetNumberProperty(texture_props, SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER, 0));
        if (!texture_id) {
            LOG_ERROR << "SDL_GetNumberProperty(SDL_PROP_TEXTURE_OPENGLES2_TEXTURE_NUMBER) failed";
            return false;
        }

        // Use the `GL_OES_EGL_image_external` extension to bind the EGLImage
        // to the texture.
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_id);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                                     reinterpret_cast<GLeglImageOES>(egl_image));

        LOG_DEBUG << std::format("Created SDL_Texture @ {} for V4L2 dmabuf on fd {}, buffer {}",
                                 (void*)buf.texture, buf.expbuf.fd, buf.expbuf.index);

        // Free the EGLImage. No longer needed after the texture has been created.
        if (eglDestroyImage(egl_display, egl_image) == EGL_FALSE) {
            LOG_ERROR << std::format("eglDestroyImage() failed with error code {:#010x}", eglGetError());
        }
    }

    // Success.
    return true;
}

bool Camera::StartCapturing()
{
    // VIDIOC_STREAMON
    enum v4l2_buf_type type(V4L2_BUF_TYPE_VIDEO_CAPTURE);
    if (ioctl(fd_, VIDIOC_STREAMON, &type) == -1) {
        LOG_ERROR << std::format("ioctl(VIDIOC_STREAMON) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Get the current file status flags of the capture file descriptor.
    auto flags = fcntl(fd_, F_GETFL);
    if (flags == -1) {
        LOG_ERROR << std::format("fcntl(F_GETFL) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Set the capture file descriptor into nonblocking mode.
    if (fcntl(fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_ERROR << std::format("fcntl(F_SETFL) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Skip the first few frames.
    for (;;) {
        struct pollfd pfd = {
            .fd = fd_,
            .events = POLLIN,
            .revents = 0,
        };

        auto status = poll(&pfd, 1, 3000 /* timeout in ms */);
        if (status == -1) {
            PushEvent(Event::CameraTimeout);
            LOG_ERROR << std::format("poll() on fd {} failed: {} ({})", fd_, errno, strerror(errno));
            return false;
        }

        if (pfd.revents & POLLIN) {
            struct v4l2_buffer buf = {};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;

            // VIDIOC_DQBUF
            if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
                LOG_ERROR << std::format("ioctl(VIDIOC_DQBUF) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
                return false;
            }

            // Save the sequence number because the VIDIOC_QBUF ioctl() below will clobber it.
            auto sequence = buf.sequence;

            // VIDIOC_QBUF
            if (ioctl(fd_, VIDIOC_QBUF, &buf) == -1) {
                LOG_ERROR << std::format("ioctl(VIDIOC_QBUF) on fd {}, buffer {} failed: {} ({})",
                                          fd_, buf.index, errno, strerror(errno));
            }

            // Break out of the loop if enough frames have been skipped.
            if (sequence >= params_.n_initial_stream_skip_frames) {
                break;
            }
        } else if (pfd.revents == 0) {
            LOG_ERROR << std::format("fd {} timed out waiting for data", fd_);
            return false;
        } else {
            LOG_ERROR << std::format("fd {} is ready but not for data, poll events {:#x}", fd_, pfd.revents);
            return false;
        }
    }

    // Put the capture file descriptor back into blocking mode.
    if (fcntl(fd_, F_SETFL, flags) == -1) {
        LOG_ERROR << std::format("fcntl(F_SETFL) on fd {} failed: {} ({})", fd_, errno, strerror(errno));
        return false;
    }

    // Success.
    return true;
}

std::shared_ptr<CameraBufferRef> Camera::NextFrame()
{
    struct v4l2_buffer buf  = {};
    buf.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory              = V4L2_MEMORY_MMAP;

    for (;;) {
        // VIDIOC_DQBUF
        if (ioctl(fd_, VIDIOC_DQBUF, &buf) == -1) {
            LOG_ERROR << std::format("ioctl(VIDIOC_DQBUF) on fd {} failed: {} ({})",
                                     fd_, errno, strerror(errno));
            return nullptr;
        }

        auto t_now = std::chrono::steady_clock::now();
        auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t_now - t_last_).count();
        s_capture_time_.Update(micros);
        t_last_ = t_now;

        // Update the v4l2_buffer embedded in the CameraBuffer in order to
        // expose the current timestamp, sequence number, etc. to the caller.
        bufs_.at(buf.index).vbuf = buf;

        // Create a new CameraBufferRef wrapping the CameraBuffer that
        // corresponds to the buffer index returned by the kernel. When this
        // object is destroyed, the buffer will be VIDIOC_QBUF'd to the kernel
        // using the Camera's V4L2 fd.
        auto bref = CameraBufferRef::Create(bufs_.at(buf.index), fd_);

        LOG_VERBOSE << std::format("Received frame on fd {}, buffer {}, sequence {}, delta {} us",
                                   fd_, bref->buf_.vbuf.index, bref->buf_.vbuf.sequence, micros);

        // If there were any errors, get another frame.
        bool is_empty_frame = buf.bytesused == 0;
        bool is_error_frame = buf.flags & V4L2_BUF_FLAG_ERROR;
        if (is_empty_frame || is_error_frame) {
            LOG_DEBUG << std::format("Discarding frame: is_empty_frame {}, is_error_frame {}",
                                     is_empty_frame, is_error_frame);
            continue;
        }

        return bref;
    }
}

std::shared_ptr<CameraBufferRef> CameraBufferRef::Create(CameraBuffer& buf, int v4l2_fd)
{
    return std::make_shared<CameraBufferRef>(CameraBufferRef(buf, v4l2_fd));
}

CameraBufferRef::CameraBufferRef(CameraBufferRef&& src)
    : buf_(src.buf_), v4l2_fd_(src.v4l2_fd_)
{
    src.v4l2_fd_ = -1;
}

CameraBufferRef::~CameraBufferRef()
{
    if (v4l2_fd_ != -1 && ioctl(v4l2_fd_, VIDIOC_QBUF, &buf_.vbuf) == -1) {
        LOG_FATAL << std::format("ioctl(VIDIOC_QBUF) on fd {}, buffer {} failed: {} ({})",
                                 v4l2_fd_, buf_.vbuf.index, errno, strerror(errno));
    }
}

} // namespace linux
} // namespace vacon
