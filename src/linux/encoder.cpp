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
#include <unordered_map>

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
    if (mfx_session_) {
        PLOG_VERBOSE << fmt::format("Closing MFX session @ {}", fmt::ptr(mfx_session_));
        MFXVideoENCODE_Close(mfx_session_);
        MFXVideoVPP_Close(mfx_session_);
        MFXClose(mfx_session_);
        mfx_session_ = nullptr;
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

    auto status = MFXCreateSession(mfx_loader_, 0 /* i */, &mfx_session_);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXCreateSession() failed: " << status;
        return false;
    }

    if (!InitMfxVideoParams()) {
        PLOG_ERROR << "InitMfxVideoParams() failed";
        return false;
    }

    if (!InitLibraryVpp()) {
        PLOG_ERROR << "InitLibraryVpp() failed";
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

bool Encoder::InitMfxVideoParams()
{
    if (!SetMfxFourCc()) {
        PLOG_ERROR << "SetMfxFourCc() failed";
        return false;
    }

    // Upload the surface data for the VPP input from system memory and put the
    // output in video memory. This allows the encoder to read the uncompressed
    // data from video memory without a roundtrip through system memory.
    mfx_videoparam_vpp_.IOPattern |= MFX_IOPATTERN_IN_SYSTEM_MEMORY;
    mfx_videoparam_vpp_.IOPattern |= MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    // Read the uncompressed input data for encoding from video memory. The VPP
    // step needs to put its output in video memory.
    mfx_videoparam_encode_.IOPattern |= MFX_IOPATTERN_IN_VIDEO_MEMORY;

    // How many asynchronous operations an application performs before the
    // application explicitly synchronizes the result. Recommended for low
    // latency.
    mfx_videoparam_encode_.AsyncDepth = 1;

    // Hint to enable low power consumption mode for encoders.
    mfx_videoparam_encode_.mfx.LowPower = MFX_CODINGOPTION_ON;

    // Specifies the codec format identifier in the FourCC code.
    mfx_videoparam_encode_.mfx.CodecId = MFX_CODEC_HEVC;

    // The codec profile.
    mfx_videoparam_encode_.mfx.CodecProfile = MFX_PROFILE_HEVC_MAIN10;
    //mfx_videoparam_encode_.mfx.CodecProfile = MFX_PROFILE_HEVC_MAIN;

    // Balanced quality and speed.
    mfx_videoparam_encode_.mfx.TargetUsage = MFX_TARGETUSAGE_BALANCED;

    // Number of pictures within each GOP (Group of Pictures).
    mfx_videoparam_encode_.mfx.GopPicSize = 60;

    // Distance between I- or P (or GPB) - key frames. If GopRefDist is 1,
    // there are no regular B-frames used (only P or GPB). Recommended for low
    // latency.
    mfx_videoparam_encode_.mfx.GopRefDist = 1;

    // Max number of all available reference frames (for AVC/HEVC, NumRefFrame
    // defines DPB size), "has the effect of only using previous P-frame as
    // reference". Recommended for low latency.
    mfx_videoparam_encode_.mfx.NumRefFrame = 1;

    // The encoder must strictly follow the given GOP structure as defined by
    // the parameters GopPicSize, GopRefDist, etc.
    mfx_videoparam_encode_.mfx.GopOptFlag |= MFX_GOP_STRICT;

    // Every I-frame is an IDR-frame.
    mfx_videoparam_encode_.mfx.IdrInterval = 1;

    // Video Conferencing Mode rate control method.
    //
    // "This algorithm is similar to VBR and uses the same set of parameters
    // InitialDelayInKB, TargetKbps, and MaxKbps. It is tuned for IPPP GOP
    // pattern and streams with strong temporal correlation between frames. It
    // produces better objective and subjective video quality in these
    // conditions than other bitrate control algorithms. It does not support
    // interlaced content, B-frames and produced stream is not HRD compliant."
    mfx_videoparam_encode_.mfx.RateControlMethod = MFX_RATECONTROL_VCM;

    // Maximum possible size of any compressed frames.
    mfx_videoparam_encode_.mfx.BufferSizeInKB = 256;

    // For CBR and VCM, used to estimate the targeted frame size by dividing
    // the frame rate by the bitrate.
    mfx_videoparam_encode_.mfx.TargetKbps = (mfxU16)params_.bitrate_kbps;

    // Frame rate numerator.
    mfx_videoparam_vpp_.vpp.In.FrameRateExtN =
    mfx_videoparam_vpp_.vpp.Out.FrameRateExtN =
    mfx_videoparam_encode_.mfx.FrameInfo.FrameRateExtN =
        (mfxU32)params_.frame_rate;

    // Frame rate denominator.
    mfx_videoparam_vpp_.vpp.In.FrameRateExtD =
    mfx_videoparam_vpp_.vpp.Out.FrameRateExtD =
    mfx_videoparam_encode_.mfx.FrameInfo.FrameRateExtD =
        1;

    // Width of the video frame in pixels. Must be a multiple of 16.
    mfx_videoparam_vpp_.vpp.In.Width =
    mfx_videoparam_vpp_.vpp.Out.Width =
    mfx_videoparam_encode_.mfx.FrameInfo.Width =
        VACON_ALIGN16((mfxU16)params_.width);

    // Height of the video frame in pixels. Must be a multiple of 16.
    mfx_videoparam_vpp_.vpp.In.Height =
    mfx_videoparam_vpp_.vpp.Out.Height =
    mfx_videoparam_encode_.mfx.FrameInfo.Height =
        VACON_ALIGN16((mfxU16)params_.height);

    // Width in pixels.
    mfx_videoparam_vpp_.vpp.In.CropW =
    mfx_videoparam_vpp_.vpp.Out.CropW =
    mfx_videoparam_encode_.mfx.FrameInfo.CropW =
        (mfxU16)params_.width;

    // Height in pixels.
    mfx_videoparam_vpp_.vpp.In.CropH =
    mfx_videoparam_vpp_.vpp.Out.CropH =
    mfx_videoparam_encode_.mfx.FrameInfo.CropH =
        (mfxU16)params_.height;

    // PicStruct
    mfx_videoparam_vpp_.vpp.In.PicStruct =
    mfx_videoparam_vpp_.vpp.Out.PicStruct =
    mfx_videoparam_encode_.mfx.FrameInfo.PicStruct =
        MFX_PICSTRUCT_PROGRESSIVE;

    // Limit the number of frames in the Decoded Picture Buffer, "to ensure
    // that decoded frame gets displayed immediately after decoding". For low
    // latency.
    mfx_eco1_.MaxDecFrameBuffering = 1;

    // Enable Reference Picture Marking Repetition SEI messages.
    //
    // "The message is used to repeat the decoded reference picture marking
    // syntax structures in the earlier decoded pictures. Consequently, even
    // earlier reference pictures were lost, the decoder can still maintain
    // correct status of the reference picture buffer and reference picture
    // lists."
    mfx_eco1_.RefPicMarkRep = MFX_CODINGOPTION_ON;

    // Enable intra refresh.
    mfx_eco2_.IntRefType = MFX_REFRESH_SLICE;

    // Encoding scenario.
    mfx_eco3_.ScenarioInfo = MFX_SCENARIO_VIDEO_CONFERENCE;

    // Attach mfx_eco's to mfx_videoparam_encode_.
    mfx_eco1_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION;
    mfx_eco2_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    mfx_eco3_.Header.BufferId = MFX_EXTBUFF_CODING_OPTION3;
    mfx_eco1_.Header.BufferSz = sizeof(mfx_eco1_);
    mfx_eco2_.Header.BufferSz = sizeof(mfx_eco2_);
    mfx_eco3_.Header.BufferSz = sizeof(mfx_eco3_);
    mfx_videoparam_encode_.ExtParam = (mfxExtBuffer**)calloc(3, sizeof(void *));
    assert(mfx_videoparam_encode_.ExtParam);
    mfx_videoparam_encode_.ExtParam[0] = (mfxExtBuffer*)&mfx_eco1_;
    mfx_videoparam_encode_.ExtParam[1] = (mfxExtBuffer*)&mfx_eco2_;
    mfx_videoparam_encode_.ExtParam[2] = (mfxExtBuffer*)&mfx_eco3_;

    // Success.
    return true;
}

bool Encoder::SetMfxFourCc()
{
    struct fourcc_mfx_params {
        mfxU32 FourCC;
        mfxU16 ChromaFormat;
        mfxU16 BitDepthChroma;
        mfxU16 BitDepthLuma;
        mfxU16 Shift;

        void ToMfxFrameInfo(mfxFrameInfo *fi) {
            fi->FourCC = FourCC;
            fi->ChromaFormat = ChromaFormat;
            fi->BitDepthChroma = BitDepthChroma;
            fi->BitDepthLuma = BitDepthLuma;
            fi->Shift = Shift;
        }
    };

    std::unordered_map<std::string, fourcc_mfx_params> map;
    map["NV12"] = { MFX_FOURCC_NV12, MFX_CHROMAFORMAT_YUV420, 8, 8, 0 };
    map["YUYV"] = { MFX_FOURCC_YUY2, MFX_CHROMAFORMAT_YUV422, 8, 8, 0 };
    map["YUY2"] = { MFX_FOURCC_YUY2, MFX_CHROMAFORMAT_YUV422, 8, 8, 0 };
    map["UYVY"] = { MFX_FOURCC_UYVY, MFX_CHROMAFORMAT_YUV422, 8, 8, 0 };
    map["P010"] = { MFX_FOURCC_P010, MFX_CHROMAFORMAT_YUV420, 10, 10, 1 };
    map["Y210"] = { MFX_FOURCC_Y210, MFX_CHROMAFORMAT_YUV422, 10, 10, 1 };

    // Set pixel format parameters.
    auto it = map.find(params_.input_pixel_format);
    if (it != map.end()) {
        PLOG_DEBUG << fmt::format("Using {} for video pixel format", it->first);

        // Set VPP input parameters to match the camera format.
        auto fourcc = it->second;
        fourcc.ToMfxFrameInfo(&mfx_videoparam_vpp_.vpp.In);

        // Set VPP output and encoder input parameters to a pixel format
        // suitable for 10-bit hardware encoding. Use a FourCC with the same
        // chroma format as the camera input format.
        if (fourcc.ChromaFormat == MFX_CHROMAFORMAT_YUV420) {
            PLOG_DEBUG << "Using P010 for encoder pixel format";
            map["P010"].ToMfxFrameInfo(&mfx_videoparam_vpp_.vpp.Out);
            map["P010"].ToMfxFrameInfo(&mfx_videoparam_encode_.mfx.FrameInfo);
        } else if (fourcc.ChromaFormat == MFX_CHROMAFORMAT_YUV422) {
            PLOG_DEBUG << "Using Y210 for encoder pixel format";
            map["Y210"].ToMfxFrameInfo(&mfx_videoparam_vpp_.vpp.Out);
            map["Y210"].ToMfxFrameInfo(&mfx_videoparam_encode_.mfx.FrameInfo);
        } else {
            PLOG_ERROR << "Unhandled chroma format: " << fourcc.ChromaFormat;
            return false;
        }
    } else {
        PLOG_ERROR << "Unhandled input pixel format: " << params_.input_pixel_format;
        return false;
    }

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

    // MFXVideoENCODE_Init(): Allocates memory and prepares tables and
    // necessary structures for encoding. This function also does extensive
    // validation to ensure if the configuration, as specified in the input
    // parameters, is supported.
    status = MFXVideoENCODE_Init(mfx_session_, &mfx_videoparam_encode_);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXVideoENCODE_Init() failed: " << status;
        return false;
    }

    // Success.
    return true;
}

bool Encoder::InitLibraryVpp()
{
    mfxConfig cfg[3];
    mfxVariant cfgVal[3];

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

    // Require that VPP scaling is supported.
    cfg[1] = MFXCreateConfig(mfx_loader_);
    if (!cfg[1]) {
        PLOG_ERROR << "MFXCreateConfig() failed";
        return false;
    }
    cfgVal[1].Type = MFX_VARIANT_TYPE_U32;
    cfgVal[1].Data.U32 = MFX_EXTBUFF_VPP_SCALING;
    status = MFXSetConfigFilterProperty(cfg[1], (mfxU8 *)"mfxImplDescription.mfxVPPDescription.filter.FilterFourCC", cfgVal[1]);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXSetConfigFilterProperty(mfxImplDescription.mfxVPPDescription.filter.FilterFourCC = MFX_EXTBUFF_VPP_SCALING) failed: " << status;
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

    status = MFXVideoVPP_Init(mfx_session_, &mfx_videoparam_vpp_);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXVideoVPP_Init() failed: " << status;
        return false;
    }

    // Success.
    return true;
}

std::shared_ptr<VideoFrame> Encoder::EncodeCameraFrame(CameraFrame& camera)
{
    auto t_start = std::chrono::steady_clock::now();

    // Initialize the frame's data.
    auto frame = std::make_shared<VideoFrame>(1024 * mfx_videoparam_encode_.mfx.BufferSizeInKB);
    frame->pts = camera.pts();

    // Create an mfxFrameSurface1 that will be used as VPP input that points
    // into the camera frame buffer.
    mfxFrameSurface1 surface_ref = {};
    if (!CameraFrameToSurface(camera, surface_ref)) {
        PLOG_ERROR << "Encoder::CameraFrameToSurface() failed";
        return nullptr;
    }

    // Issue the VPP scaling request to the GPU.
    auto status =
        MFXVideoVPP_ProcessFrameAsync(mfx_session_,
                                      &surface_ref,
                                      &frame->surface);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXVideoVPP_RunFrameVPPAsync() failed: " << status;
        return nullptr;
    }

    // Issue the encoding request to the GPU.
    mfxSyncPoint syncp = {};
    status =
        MFXVideoENCODE_EncodeFrameAsync(mfx_session_,
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
    bool stalled = false;
    do {
        status = MFXVideoCORE_SyncOperation(mfx_session_, syncp, 10 /* wait ms */);
        if (status == MFX_WRN_IN_EXECUTION) {
            stalled = true;

        }
    } while (status == MFX_WRN_IN_EXECUTION);
    if (status != MFX_ERR_NONE) {
        PLOG_ERROR << "MFXVideoCORE_SyncOperation() failed: " << status;
        return nullptr;
    }

    auto t_stop = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t_stop - t_start).count();
    auto msg = fmt::format("Encoded frame in {} us, {} bytes", micros, frame->bitstream.DataLength);
    if (stalled) {
        PLOG_DEBUG << msg;
    } else {
        PLOG_VERBOSE << msg;
    }

    // Success.
    return frame;
}

bool Encoder::CameraFrameToSurface(const CameraFrame& camera, mfxFrameSurface1& surface)
{
    const mfxFrameInfo& info = mfx_videoparam_vpp_.vpp.In;
    auto width = info.CropW;
    auto height = info.CropH;

    size_t bytes_needed = 0;

    switch (camera.fourcc_) {
    case V4L2_PIX_FMT_NV12:
        bytes_needed = width * height * 3 / 2;
        surface.Data.Y = reinterpret_cast<mfxU8*>(camera.data_);
        surface.Data.UV = surface.Data.Y + width * height;
        surface.Data.Pitch = width;
        break;

    case V4L2_PIX_FMT_YUYV:
        bytes_needed = width * height * 2;
        surface.Data.Y = reinterpret_cast<mfxU8*>(camera.data_);
        surface.Data.U = surface.Data.Y + 1;
        surface.Data.V = surface.Data.Y + 3;
        surface.Data.Pitch = width * 2;
        break;

    case V4L2_PIX_FMT_UYVY:
        bytes_needed = width * height * 2;
        surface.Data.U = reinterpret_cast<mfxU8*>(camera.data_);
        surface.Data.Y = surface.Data.U + 1;
        surface.Data.V = surface.Data.U + 2;
        surface.Data.Pitch = width * 2;
        break;

    default:
        PLOG_ERROR << fmt::format("Unsupported camera frame FourCC {} ({:#010x})",
                                  FourCcToString(camera.fourcc_), camera.fourcc_);
        return false;
    }

    if (bytes_needed != (size_t)camera.buf_.bytesused) {
        PLOG_ERROR << fmt::format("Camera frame is {} bytes, but MFX surface needs {} bytes",
                                  camera.buf_.bytesused, bytes_needed);
        return false;
    }

    surface.Info = info;

    // Success.
    return true;
}

} // namespace linux
} // namespace vacon
