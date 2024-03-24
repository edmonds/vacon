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

#include "linux/decoder.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <format>
#include <memory>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_egl.h>
#include <SDL3/SDL_opengles2.h>
#include <libdrm/drm_fourcc.h>
#include <mfx.h>
#include <plog/Log.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_str.h>
#include <va/va_wayland.h>
#include <wayland-client.h>

#include "event.hpp"
#include "linux/mfx.hpp"
#include "rtc_packet.hpp"
#include "util.hpp"

using namespace std::chrono_literals;

namespace vacon {
namespace linux {

std::atomic_size_t n_frames_decode_success  = 0;
std::atomic_size_t n_frames_decode_fail     = 0;
std::atomic_size_t n_frames_decode_overflow = 0;

std::unique_ptr<Decoder> Decoder::Create(const DecoderParams& params)
{
    return std::make_unique<Decoder>(Decoder(params));
}

Decoder::~Decoder()
{
    RequestStop();
    Join();

    if (mfx_session_) {
        LOG_VERBOSE << std::format("Closing MFX session @ {}", (void*)mfx_session_);
        MFXVideoDECODE_Close(mfx_session_);
        MFXClose(mfx_session_);
        mfx_session_ = nullptr;
    }

    if (mfx_loader_) {
        LOG_VERBOSE << std::format("Unloading MFX loader @ {}", (void*)mfx_loader_);
        MFXUnload(mfx_loader_);
        mfx_loader_ = nullptr;
    }

    if (va_display_) {
        LOG_VERBOSE << std::format("Terminating VADisplay @ {}", (void*)va_display_);
        vaTerminate(va_display_);
        va_display_ = nullptr;
    }

    if (wl_display_) {
        LOG_VERBOSE << std::format("Disconnecting Wayland display @ {}", (void*)wl_display_);
        wl_display_disconnect(wl_display_);
        wl_display_ = nullptr;
    }
}

bool Decoder::Init()
{
    thread_ = std::jthread([&](std::stop_token st) { RunDecoder(st); });
    return true;
}

void Decoder::RequestStop()
{
    if (thread_.joinable()) {
        LOG_DEBUG << "Requesting stop of decoder thread ID " << thread_.get_id();
        thread_.request_stop();
    }
}

void Decoder::Join()
{
    if (thread_.joinable()) {
        LOG_DEBUG << "Joining decoder thread ID " << thread_.get_id();
        thread_.join();
        thread_ = {};
    }
}

void Decoder::RunDecoder(std::stop_token st)
{
    LOG_DEBUG << "Starting video decoder thread ID " << std::this_thread::get_id();
    util::SetThreadName("VDecoderVideo");

    PushEvent(Event::DecoderStarting);
    if (!InitDecoder()) {
        LOG_ERROR << "InitDecoder() failed!";
        PushEvent(Event::DecoderFailed);
        return;
    }
    PushEvent(Event::DecoderStarted);

    while (!st.stop_requested()) {
        std::shared_ptr<RtcPacket> packet;

        if (params_.incoming_video_packet_queue->wait_dequeue_timed(packet, 10ms)) {
            DecodePacket(packet);
        } else {
            LOG_VERBOSE << "Stalled dequeuing packet from incoming video packet queue, retrying";
        }
    }

    LOG_DEBUG << "Stopping video decoder thread ID " << std::this_thread::get_id();
}

bool Decoder::InitDecoder()
{
    auto t_start = std::chrono::steady_clock::now();

    mfx_loader_ = MFXLoad();
    if (!mfx_loader_) {
        LOG_ERROR << "MFXLoad() failed";
        return false;
    }

    mfx_videoparam_decode_.mfx.CodecId = MFX_CODEC_HEVC;
    mfx_videoparam_decode_.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    auto status = MFXCreateSession(mfx_loader_, 0 /* i */, &mfx_session_);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "MFXCreateSession() failed: " << MfxStatusStr(status);
        return false;
    }

    if (!SetMfxLoaderConfigFilters(mfx_loader_,
        {
            { "mfxImplDescription.AccelerationMode", MFX_ACCEL_MODE_VIA_VAAPI },
            { "mfxImplDescription.ApiVersion.Version", ((2 << 16) | 9) },
            { "mfxImplDescription.Impl", MFX_IMPL_TYPE_HARDWARE },
            { "mfxImplDescription.mfxDecoderDescription.decoder.CodecID", mfx_videoparam_decode_.mfx.CodecId },
        }))
    {
        LOG_ERROR << "SetMfxLoaderConfigFilters() failed";
        return false;
    }

    if (!SetMfxLoaderConfigFiltersCombined(mfx_loader_,
        {
            { "mfxSurfaceTypesSupported.surftype.SurfaceType", MFX_SURFACE_TYPE_VAAPI },
            { "mfxSurfaceTypesSupported.surftype.surfcomp.SurfaceComponent", MFX_SURFACE_COMPONENT_DECODE },
            { "mfxSurfaceTypesSupported.surftype.surfcomp.SurfaceFlags", MFX_SURFACE_FLAG_EXPORT_SHARED },
        }))
    {
        LOG_ERROR << "SetMfxLoaderConfigFilters() failed";
        return false;
    }

    if (!InitVaapi()) {
        LOG_ERROR << "InitVaapi() failed";
        return false;
    }

    auto t_end = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    LOG_INFO << std::format("Initialized video decoder in {} ms", millis);

    return true;
}

bool Decoder::InitVaapi()
{
    // Connect to the Wayland compositor.
    wl_display_ = wl_display_connect(nullptr);
    if (!wl_display_) {
        LOG_ERROR << "wl_display_connect() failed";
        return false;
    }

    // Get a VADisplay from the Wayland compositor.
    va_display_ = vaGetDisplayWl(wl_display_);
    if (!va_display_) {
        LOG_ERROR << "vaGetDisplayWl() failed";
        return false;
    }

    // Initialize the VADisplay.
    int major = 0, minor = 0;
    auto va_status = vaInitialize(va_display_, &major, &minor);
    if (va_status != VA_STATUS_SUCCESS) {
        LOG_ERROR << std::format("vaInitialize() failed: {} ({})",
                                 vaStatusStr(va_status), va_status);
        return false;
    }
    LOG_VERBOSE << std::format("Initialized VADisplay @ {}", va_display_);

    // Pass the VADisplay to the MFX library.
    auto mfx_status = MFXVideoCORE_SetHandle(mfx_session_,
                                             MFX_HANDLE_VA_DISPLAY,
                                             static_cast<mfxHDL>(va_display_));
    if (mfx_status != MFX_ERR_NONE) {
        LOG_ERROR << "MFXVideoCORE_SetHandle() failed: " << MfxStatusStr(mfx_status);
        return false;
    }

    // Success.
    return true;
}

void Decoder::DecodePacket(std::shared_ptr<RtcPacket> rtc_packet)
{
    auto t_start = std::chrono::steady_clock::now();

    mfxStatus status;

    mfxBitstream bitstream  = {};
    bitstream.CodecId       = mfx_videoparam_decode_.mfx.CodecId;
    bitstream.Data          = reinterpret_cast<mfxU8*>(rtc_packet->msg_.data());
    bitstream.DataFlag      = MFX_BITSTREAM_COMPLETE_FRAME;
    bitstream.DataLength    = rtc_packet->msg_.size();
    bitstream.MaxLength     = rtc_packet->msg_.size();

    // If the decoder needs to be initialized, decode the header of this frame
    // and then initialize the decoder.
    if (need_decode_init_) {
        status =
            MFXVideoDECODE_DecodeHeader(mfx_session_,
                                        &bitstream,
                                        &mfx_videoparam_decode_);
        if (status == MFX_ERR_NONE) {
            status = MFXVideoDECODE_Init(mfx_session_, &mfx_videoparam_decode_);
            if (status == MFX_ERR_NONE) {
                need_decode_init_ = false;
                LOG_DEBUG << std::format("MFXVideoDECODE_Init() succeeded,"
                                         " output pixel format {}, chroma format {},"
                                         " bit depth chroma {}, bit depth luma {}, shift {},"
                                         " width {}, height {}, cropw {}, croph {}",
                                         util::FourCcToString(mfx_videoparam_decode_.mfx.FrameInfo.FourCC),
                                         mfx_videoparam_decode_.mfx.FrameInfo.ChromaFormat,
                                         mfx_videoparam_decode_.mfx.FrameInfo.BitDepthChroma,
                                         mfx_videoparam_decode_.mfx.FrameInfo.BitDepthLuma,
                                         mfx_videoparam_decode_.mfx.FrameInfo.Shift,
                                         mfx_videoparam_decode_.mfx.FrameInfo.Width,
                                         mfx_videoparam_decode_.mfx.FrameInfo.Height,
                                         mfx_videoparam_decode_.mfx.FrameInfo.CropW,
                                         mfx_videoparam_decode_.mfx.FrameInfo.CropH);
            } else {
                LOG_DEBUG << "MFXVideoDECODE_Init() failed: " << MfxStatusStr(status);
                return;
            }
        } else {
            LOG_DEBUG << "MFXVideoDECODE_DecodeHeader() failed: " << MfxStatusStr(status);
            n_frames_decode_fail.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // Submit the bitstream to be decoded.
    auto frame = std::make_shared<DecodedFrame>();
    mfxSyncPoint syncp = {};
    status =
        MFXVideoDECODE_DecodeFrameAsync(mfx_session_,
                                        &bitstream,
                                        nullptr,
                                        &frame->surface_,
                                        &syncp);
    if (status == MFX_WRN_VIDEO_PARAM_CHANGED) {
        // Submit the bitstream to be decoded *again*.
        status =
            MFXVideoDECODE_DecodeFrameAsync(mfx_session_,
                                            &bitstream,
                                            nullptr,
                                            &frame->surface_,
                                            &syncp);
        if (status != MFX_ERR_NONE) {
            // Terminate the decoding operation and re-initialize it next time.
            LOG_ERROR << std::format("MFXVideoDECODE_DecodeFrameAsync() failed with {}"
                                     " after MFX_WRN_VIDEO_PARAM_CHANGED, resetting decoder",
                                     MfxStatusStr(status));
            MFXVideoDECODE_Close(mfx_session_);
            need_decode_init_ = true;
            return;
        }
    } else if (status != MFX_ERR_NONE) {
        n_frames_decode_fail.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR << "MFXVideoDECODE_DecodeFrameAsync() failed: " << MfxStatusStr(status);
        return;
    }

    // Wait for the decoding request to complete and return the decoded frame.
    do {
        status = MFXVideoCORE_SyncOperation(mfx_session_, syncp, 10 /* wait ms */);
    } while (status == MFX_WRN_IN_EXECUTION);
    if (status == MFX_ERR_NONE) {
        n_frames_decode_success.fetch_add(1, std::memory_order_relaxed);
    } else {
        n_frames_decode_fail.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR << "MFXVideoCORE_SyncOperation() failed: " << MfxStatusStr(status);
        return;
    }

    // Export the decoded frame to a VAAPI surface.
    mfxSurfaceHeader export_header      = {};
    export_header.SurfaceType           = MFX_SURFACE_TYPE_VAAPI;
    export_header.SurfaceFlags          = MFX_SURFACE_FLAG_EXPORT_SHARED;
    mfxSurfaceHeader* exported_surface  = nullptr;
    status = frame->surface_->FrameInterface->Export(frame->surface_, export_header, &exported_surface);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "FrameInterface::Export() failed: " << MfxStatusStr(status);
        return;
    }
    frame->exported_surface_ = reinterpret_cast<mfxSurfaceVAAPI*>(exported_surface);

    // Export the VAAPI surface to a DRM PRIME file descriptor.
    auto
        va_status = vaExportSurfaceHandle(frame->exported_surface_->vaDisplay,
                                          frame->exported_surface_->vaSurfaceID,
                                          VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                          &frame->prime_);
    if (va_status != VA_STATUS_SUCCESS) {
        LOG_ERROR << std::format("vaExportSurfaceHandle() failed: {} ({})",
                                 vaStatusStr(va_status), va_status);
        return;
    }

    // vaSyncSurface() must be called before reading from the exported surface.
    va_status = vaSyncSurface(frame->exported_surface_->vaDisplay,
                              frame->exported_surface_->vaSurfaceID);
    if (va_status != VA_STATUS_SUCCESS) {
        LOG_ERROR << std::format("vaSyncSurface() failed: {} ({})",
                                 vaStatusStr(va_status), va_status);
        return;
    }

    auto t_end = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count();
    s_decode_time_.Update(micros);
    LOG_VERBOSE << std::format("Decoded video packet in {} us", micros);

    // Enqueue the decoded video frame onto the queue for the renderer.
    if (params_.decoded_video_frame_queue) {
        if (!params_.decoded_video_frame_queue->try_enqueue(frame)) {
            LOG_DEBUG << "Failed to enqueue frame onto decoder output queue, discarding!";
            n_frames_decode_overflow.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

DecodedFrame::DecodedFrame(DecodedFrame&& src)
{
    surface_                = src.surface_;
    exported_surface_       = src.exported_surface_;
    prime_                  = src.prime_;
    texture_                = src.texture_;

    src.surface_            = nullptr;
    src.exported_surface_   = nullptr;
    src.prime_              = {};
    src.texture_            = nullptr;
}

DecodedFrame::~DecodedFrame()
{
    if (texture_) {
        LOG_VERBOSE << std::format("Destroying SDL_Texture @ {}", (void*)texture_);
        SDL_ClearError();
        SDL_DestroyTexture(texture_);
        if (auto err = std::string(SDL_GetError()); err != "") {
            LOG_ERROR << std::format("SDL_DestroyTexture() failed: {}", err);
        }
        texture_ = nullptr;
    }

    for (uint32_t i = 0; i < prime_.num_objects; ++i) {
        int fd = prime_.objects[i].fd;
        LOG_VERBOSE << "Closing DRM PRIME fd " << fd;
        if (close(fd) != 0) {
            LOG_ERROR << std::format("close() failed on DRM PRIME fd {}: {} ({})",
                                     fd, errno, strerror(errno));
        }
    }
    prime_ = {};

    if (exported_surface_) {
        LOG_VERBOSE << "Releasing VASurfaceID " << exported_surface_->vaSurfaceID;
        auto status = exported_surface_->SurfaceInterface.Release(&(exported_surface_->SurfaceInterface));
        if (status != MFX_ERR_NONE) {
            LOG_ERROR << "SurfaceInterface::Release() failed: " << MfxStatusStr(status);
        }
        exported_surface_ = nullptr;
    }

    if (surface_) {
        auto status = surface_->FrameInterface->Release(surface_);
        if (status != MFX_ERR_NONE) {
            LOG_ERROR << "FrameInterface::Release() failed: " << MfxStatusStr(status);
        }
        surface_ = nullptr;
    }
}

bool DecodedFrame::ExportToOpenGL(SDL_Renderer *sdl_renderer)
{
    switch (prime_.fourcc) {
    case MFX_FOURCC_P010:
        break;
    default:
        LOG_ERROR << std::format("Unhandled DRM pixel format {} ({:#010x})",
                                 util::FourCcToString(prime_.fourcc),
                                 prime_.fourcc);
        return false;
    }

    // Get the current EGL display.
    auto egl_display = eglGetCurrentDisplay();
    if (egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR << std::format("eglGetCurrentDisplay() failed with error code {:#010x}", eglGetError());
        return false;
    }

    // Get the DRM format modifier.
    const auto drm_format_modifier_lo =
        static_cast<EGLint>((prime_.objects[0].drm_format_modifier >> 0) & 0xFFFFFFFF);
    const auto drm_format_modifier_hi =
        static_cast<EGLint>((prime_.objects[0].drm_format_modifier >> 32) & 0xFFFFFFFF);

    // Construct the attribute list needed to create an EGLImage using the
    // `EGL_EXT_image_dma_buf_import` extension.
    std::vector<EGLAttrib> attrs = {
        EGL_LINUX_DRM_FOURCC_EXT,           prime_.fourcc,
        EGL_WIDTH,                          surface_->Info.CropW,
        EGL_HEIGHT,                         surface_->Info.CropH,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,       prime_.layers[0].pitch[0],
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,      prime_.layers[0].offset[0],
        EGL_DMA_BUF_PLANE0_FD_EXT,          prime_.objects[0].fd,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, drm_format_modifier_lo,
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, drm_format_modifier_hi,
    };

    if (prime_.fourcc == DRM_FORMAT_P010) {
        // P010 is a "semi-planar" format and needs additional attributes
        // specifying the UV plane.
        std::vector<EGLAttrib> more_attrs = {
            EGL_DMA_BUF_PLANE1_PITCH_EXT,       prime_.layers[0].pitch[1],
            EGL_DMA_BUF_PLANE1_OFFSET_EXT,      prime_.layers[0].offset[1],
            EGL_DMA_BUF_PLANE1_FD_EXT,          prime_.objects[0].fd,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, drm_format_modifier_lo,
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, drm_format_modifier_hi,
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
    texture_ =
        SDL_CreateTexture(sdl_renderer,
                          SDL_PIXELFORMAT_EXTERNAL_OES,
                          SDL_TEXTUREACCESS_STATIC,
                          surface_->Info.CropW,
                          surface_->Info.CropH);
    if (!texture_) {
        LOG_ERROR << "SDL_CreateTexture() failed: " << SDL_GetError();
        return false;
    }
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_BEST);

    // Get the texture properties.
    auto texture_props = SDL_GetTextureProperties(texture_);
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

    // Use the `GL_OES_EGL_image_external` extension to bind the EGLImage to
    // the texture.
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture_id);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                                 reinterpret_cast<GLeglImageOES>(egl_image));

    LOG_VERBOSE << std::format("Created SDL_Texture @ {} for DRM PRIME fd {}",
                               (void*)texture_, prime_.objects[0].fd);

    // Free the EGLImage. No longer needed after the texture has been created.
    if (eglDestroyImage(egl_display, egl_image) == EGL_FALSE) {
        LOG_ERROR << std::format("eglDestroyImage() failed with error code {:#010x}", eglGetError());
    }

    // Success.
    return true;
}

} // namespace linux
} // namespace vacon
