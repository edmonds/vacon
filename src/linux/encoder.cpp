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

// Parts derived from Intel® Video Processing Library (Intel® VPL) 2.X
// "hello-encode" example:
//
// https://github.com/intel/libvpl/tree/master/examples/api2x/hello-encode
//
// Copyright (C) Intel Corporation
//
// SPDX-License-Identifier: MIT

#include "encoder.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <fmt/format.h>

#include <mfx.h>

#include "../common.hpp"
#include "video_frame.hpp"

namespace vacon {
namespace linux {

std::shared_ptr<Encoder> Encoder::Create(const EncoderParams& params)
{
    if (params.input_pixel_format == "") {
        PLOG_ERROR << "Camera pixel format must be specified";
        return nullptr;
    }

    auto enc = std::make_shared<Encoder>(Encoder {});
    enc->params_ = params;

    // Convert pixel format parameter to uppercase.
    std::transform(enc->params_.input_pixel_format.begin(),
                   enc->params_.input_pixel_format.end(),
                   enc->params_.input_pixel_format.begin(),
                   ::toupper);

    return enc;
}

Encoder::~Encoder()
{
    if (mfx_session_encode_) {
        PLOG_VERBOSE << fmt::format("Closing MFX ENCODE session @ {}", fmt::ptr(mfx_session_encode_));
        MFXVideoENCODE_Close(mfx_session_encode_);
        MFXClose(mfx_session_encode_);
        mfx_session_encode_ = nullptr;
    }

    if (mfx_loader_) {
        PLOG_VERBOSE << fmt::format("Unloading MFX loader @ {}", fmt::ptr(mfx_loader_));
        MFXUnload(mfx_loader_);
        mfx_loader_ = nullptr;
    }

    free(mfx_videoparam_encode_.ExtParam);
}

bool Encoder::Init()
{
    PLOG_DEBUG <<
        fmt::format("EncoderParams: input pixel format '{}', width {}, height {}, frame rate {}, bitrate {}",
                    params_.input_pixel_format,
                    params_.width,
                    params_.height,
                    params_.frame_rate,
                    params_.bitrate_kbps);

    auto t_start = std::chrono::steady_clock::now();

    mfx_loader_ = MFXLoad();
    if (!mfx_loader_) {
        PLOG_ERROR << "MFXLoad() failed";
        return false;
    }

    if (!InitMfxVideoParamEncode()) {
        PLOG_ERROR << "InitMfxVideoParamEncode() failed";
        return false;
    }

    if (!InitLibraryEncode()) {
        PLOG_ERROR << "InitLibraryEncode() failed";
        return false;
    }

    auto t_end = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    PLOG_INFO << fmt::format("Initialized video encoder in {} ms", millis);

    return true;
}

bool Encoder::InitMfxVideoParamEncode()
{
    // How many asynchronous operations an application performs before the
    // application explicitly synchronizes the result.
    mfx_videoparam_encode_.AsyncDepth = 1;

    // Input to functions is a video memory surface.
    mfx_videoparam_encode_.IOPattern |= MFX_IOPATTERN_IN_SYSTEM_MEMORY;

    // Hint to enable low power consumption mode for encoders.
    mfx_videoparam_encode_.mfx.LowPower = MFX_CODINGOPTION_ON;

    // Specifies the codec format identifier in the FourCC code.
    mfx_videoparam_encode_.mfx.CodecId = MFX_CODEC_HEVC;

    // The codec profile.
    //mfx_videoparam_encode_.mfx.CodecProfile = MFX_PROFILE_HEVC_MAIN10;
    //mfx_videoparam_encode_.mfx.CodecProfile = MFX_PROFILE_HEVC_MAIN;

    // Balanced quality and speed.
    mfx_videoparam_encode_.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;

    // Number of pictures within each GOP (Group of Pictures).
    mfx_videoparam_encode_.mfx.GopPicSize = 60;

    // Distance between I- or P (or GPB) - key frames. If GopRefDist is 1,
    // there are no regular B-frames used (only P or GPB).
    mfx_videoparam_encode_.mfx.GopRefDist = 1;

    // The encoder must strictly follow the given GOP structure as defined by
    // the parameters GopPicSize, GopRefDist, etc.
    mfx_videoparam_encode_.mfx.GopOptFlag |= MFX_GOP_STRICT;

    // Every I-frame is an IDR-frame.
    mfx_videoparam_encode_.mfx.IdrInterval = 1;

    // Constant bitrate control algorithm.
    mfx_videoparam_encode_.mfx.RateControlMethod = MFX_RATECONTROL_CBR;

    // Maximum possible size of any compressed frames.
    mfx_videoparam_encode_.mfx.BufferSizeInKB = 256;

    // For CBR, used to estimate the targeted frame size by dividing the frame
    // rate by the bitrate.
    mfx_videoparam_encode_.mfx.TargetKbps = (mfxU16)params_.bitrate_kbps;

    // Frame rate numerator.
    mfx_videoparam_encode_.mfx.FrameInfo.FrameRateExtN = (mfxU32)params_.frame_rate;

    // Frame rate denominator.
    mfx_videoparam_encode_.mfx.FrameInfo.FrameRateExtD = 1;

    // Width of the video frame in pixels. Must be a multiple of 16.
    mfx_videoparam_encode_.mfx.FrameInfo.Width = VACON_ALIGN16((mfxU16)params_.width);

    // Height of the video frame in pixels. Must be a multiple of 16.
    mfx_videoparam_encode_.mfx.FrameInfo.Height = VACON_ALIGN16((mfxU16)params_.height);

    // Width in pixels.
    mfx_videoparam_encode_.mfx.FrameInfo.CropW = (mfxU16)params_.width;

    // Height in pixels.
    mfx_videoparam_encode_.mfx.FrameInfo.CropH = (mfxU16)params_.height;

    // Pixel color format values dependent on the configured pixel format.
    if (params_.input_pixel_format == "NV12") {
        mfx_videoparam_encode_.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
        mfx_videoparam_encode_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    } else if (params_.input_pixel_format == "YUY2" ||
               params_.input_pixel_format == "YUYV422")
    {
        //mfx_videoparam_encode_.mfx.FrameInfo.FourCC = MFX_FOURCC_YUY2;
        //mfx_videoparam_encode_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV422;
        PLOG_ERROR << "Camera pixel format YUY2 not supported right now";
        return false;
    } else if (params_.input_pixel_format == "UYVY" ||
               params_.input_pixel_format == "UYVY422")
    {
        //mfx_videoparam_encode_.mfx.FrameInfo.FourCC = MFX_FOURCC_UYVY;
        //mfx_videoparam_encode_.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV422;
        PLOG_ERROR << "Camera pixel format UYVY not supported right now";
        return false;
    } else {
        PLOG_ERROR << "Unknown camera pixel format: " << params_.input_pixel_format;
        return false;
    }

    // Try to enable intra refresh.
    mfx_eco2_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    mfx_eco2_.Header.BufferSz = sizeof(mfx_eco2_);
    mfx_eco2_.IntRefType = MFX_REFRESH_SLICE;

    // Try to set the encoding scenario.
    mfx_eco3_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
    mfx_eco3_.Header.BufferSz = sizeof(mfx_eco3_);
    mfx_eco3_.ScenarioInfo = MFX_SCENARIO_VIDEO_CONFERENCE;

    // Attach mfx_eco's to mfx_videoparam_encode_.
    mfx_videoparam_encode_.ExtParam = (mfxExtBuffer**)calloc(2, sizeof(void *));
    assert(mfx_videoparam_encode_.ExtParam);
    mfx_videoparam_encode_.ExtParam[0] = (mfxExtBuffer*)&mfx_eco2_;
    mfx_videoparam_encode_.ExtParam[1] = (mfxExtBuffer*)&mfx_eco3_;

    // Success.
    return true;
}

bool Encoder::InitLibraryEncode()
{
    mfxConfig cfg[3];
    mfxVariant cfgVal[3];

    // MFXCreateConfig(): Creates the dispatcher internal configuration, which
    // is used to filter out available implementations. This configuration is
    // used to walk through selected implementations to gather more details and
    // select the appropriate implementation to load. The loader object
    // remembers all created mfxConfig objects and destroys them during the
    // mfxUnload function call.
    //
    // MFXSetConfigFilterProperty(): Adds additional filter properties (any
    // fields of the mfxImplDescription structure) to the configuration of the
    // loader object. One mfxConfig properties can hold only single filter
    // property.

    // Require a hardware video encoder.
    cfg[0] = MFXCreateConfig(mfx_loader_);
    if(!cfg[0]) {
        PLOG_ERROR << "MFXCreateConfig() failed";
        return false;
    }
    cfgVal[0].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[0].Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    auto status = MFXSetConfigFilterProperty(cfg[0], (mfxU8 *)"mfxImplDescription.Impl", cfgVal[0]);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXSetConfigFilterProperty(mfxImplDescription.Impl = MFX_IMPL_TYPE_HARDWARE) failed: " << status;
        return false;
    }

    // Require a particular codec.
    cfg[1] = MFXCreateConfig(mfx_loader_);
    if (!cfg[1]) {
        PLOG_ERROR << "MFXCreateConfig() failed";
        return false;
    }
    cfgVal[1].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[1].Data.U32 = MFX_CODEC_HEVC;
    status = MFXSetConfigFilterProperty(cfg[1], (mfxU8 *)"mfxImplDescription.mfxEncoderDescription.encoder.CodecID", cfgVal[1]);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXSetConfigFilterProperty(mfxImplDescription.mfxEncoderDescription.encoder.CodecID = MFX_CODEC_HEVC) failed: " << status;
        return false;
    }

    // Require API version >= 2.2.
    cfg[2] = MFXCreateConfig(mfx_loader_);
    if (!cfg[2]) {
        PLOG_ERROR << "MFXCreateConfig() failed";
        return false;
    }
    cfgVal[2].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[2].Data.U32 = ((2 << 16) | 2);
    status = MFXSetConfigFilterProperty(cfg[2], (mfxU8 *)"mfxImplDescription.ApiVersion.Version", cfgVal[2]);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXSetConfigFilterProperty(mfxImplDescription.ApiVersion.Version >= 2.2) failed: " << status;
        return false;
    }

    // MFXCreateSession(): Loads and initializes the implementation.
    status = MFXCreateSession(mfx_loader_, 0 /* i */, &mfx_session_encode_);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXCreateSession() failed: " << status;
        return false;
    }

    // MFXVideoENCODE_Init(): Allocates memory and prepares tables and
    // necessary structures for encoding. This function also does extensive
    // validation to ensure if the configuration, as specified in the input
    // parameters, is supported.
    status = MFXVideoENCODE_Init(mfx_session_encode_, &mfx_videoparam_encode_);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXVideoENCODE_Init() failed: " << status;
        return false;
    }

    // Success.
    return true;
}

std::shared_ptr<VideoFrame> Encoder::EncodeCameraFrame(CameraFrame& camera)
{
    auto t_start = std::chrono::steady_clock::now();

    // Consistency check.
    assert(camera.fourcc_ == mfx_videoparam_encode_.mfx.FrameInfo.FourCC);

    // Initialize the data for the encoded frame.
    auto frame = std::make_shared<VideoFrame>(1024 * mfx_videoparam_encode_.mfx.BufferSizeInKB);
    frame->pts = camera.pts();

    // Get a new surface to upload the frame data from the CPU to the GPU.
    auto status = MFXMemory_GetSurfaceForEncode(mfx_session_encode_, &frame->surface);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXMemory_GetSurfaceForEncode() failed: " << status;
        return nullptr;
    }

    // Map the surface onto the CPU.
    status = frame->surface->FrameInterface->Map(frame->surface, MFX_MAP_WRITE);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "mfxFrameSurfaceInterface->Map() failed: " << status;
        return nullptr;
    }

    // Copy the camera frame data to the surface.
    if (!frame->CopyCameraFrameToSurface(camera)) {
        PLOG_ERROR << "frame->CopyCameraFrameToSurface() failed";
        return nullptr;
    }

    // Unmap the surface from the CPU. This uploads the data to the GPU?
    status = frame->surface->FrameInterface->Unmap(frame->surface);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "mfxFrameSurfaceInterface->Unmap() failed: " << status;
        return nullptr;
    }

    // Issue the encoding request to the GPU.
    mfxSyncPoint syncp = {};
    status = MFXVideoENCODE_EncodeFrameAsync(mfx_session_encode_,
                                             nullptr /* ctrl */,
                                             frame->surface,
                                             &frame->bitstream,
                                             &syncp);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXVideoENCODE_EncodeFrameAsync() failed: " << status;
        return nullptr;
    }

    // Check status of encoding request.
    if (!syncp) {
        PLOG_ERROR << "MFXVideoENCODE_EncodeFrameAsync() failed to return a synchronization point";
        return nullptr;
    }

    // Wait for the encoding request to complete and return the encoded frame.
    do {
        status = MFXVideoCORE_SyncOperation(mfx_session_encode_, syncp, 10 /* wait ms */);
    } while (status == MFX_WRN_IN_EXECUTION);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXVideoCORE_SyncOperation() failed: " << status;
        return nullptr;
    }

    auto t_stop = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t_stop - t_start).count();
    PLOG_VERBOSE << fmt::format("Encoded frame in {} us, {} bytes", micros, frame->bitstream.DataLength);

    // Success.
    return frame;
}

} // namespace linux
} // namespace vacon
