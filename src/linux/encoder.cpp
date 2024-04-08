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

#include "linux/encoder.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include <mfx.h>
#include <plog/Log.h>

#include "codecs.hpp"
#include "event.hpp"
#include "linux/camera.hpp"
#include "linux/mfx.hpp"
#include "linux/mfx_loader.hpp"
#include "linux/video_frame.hpp"
#include "util.hpp"

using namespace std::chrono_literals;

namespace vacon {
namespace linux {

std::atomic_size_t n_frames_encode_success  = 0;
std::atomic_size_t n_frames_encode_fail     = 0;
std::atomic_size_t n_frames_encode_stall    = 0;

std::unique_ptr<Encoder> Encoder::Create(const EncoderParams& params)
{
    auto t_start = std::chrono::steady_clock::now();

    if (!params.encoder_queue) {
        LOG_ERROR << "Encoder CameraBufferQueue must be provided";
        return nullptr;
    }

    auto enc = std::make_unique<Encoder>(Encoder(params));
    enc->mfx_session_ = GetMfxSession();
    if (!enc->mfx_session_) {
        LOG_ERROR << "GetMfxSession() failed";
        return nullptr;
    }

    auto t_end = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    LOG_INFO << std::format("Created video encoder in {} ms", millis);

    return enc;
}

Encoder::~Encoder()
{
    RequestStop();
    Join();

    if (mfx_session_) {
        LOG_VERBOSE << std::format("Closing MFX session @ {}", (void*)mfx_session_);
        MFXVideoENCODE_Close(mfx_session_);
        MFXVideoVPP_Close(mfx_session_);
        MFXClose(mfx_session_);
        mfx_session_ = nullptr;
    }

    free(mfx_videoparam_encode_.ExtParam);
}

void Encoder::StartThread(VideoCodec codec, const CameraFormat& fmt)
{
    codec_ = codec;
    camera_format_ = fmt;
    thread_ = std::jthread([&](std::stop_token st) { RunEncoder(st); });
}

void Encoder::RequestStop()
{
    if (thread_.joinable()) {
        LOG_DEBUG << "Requesting stop of encoder thread ID " << thread_.get_id();
        thread_.request_stop();
    }
}

void Encoder::Join()
{
    if (thread_.joinable()) {
        LOG_DEBUG << "Joining encoder thread ID " << thread_.get_id();
        thread_.join();
        thread_ = {};
    }
}

std::shared_ptr<std::vector<VideoCodec>> Encoder::GetSupportedCodecs(std::optional<VideoCodec> force)
{
    supported_pixel_formats_.clear();

    auto codecs = std::make_shared<std::vector<VideoCodec>>();
    auto codecs_set = std::set<VideoCodec>{};

    auto mfx_loader = MfxLoader::GetInstance();
    if (!mfx_loader) {
        LOG_ERROR << "MfxLoader::GetInstance() failed";
        return codecs;
    }

    int impl_idx = 0;
    mfxImplDescription *desc = nullptr;
    while (MFXEnumImplementations(mfx_loader->Get(), impl_idx++,
                                  MFX_IMPLCAPS_IMPLDESCSTRUCTURE,
                                  reinterpret_cast<mfxHDL*>(&desc)) == MFX_ERR_NONE)
    {
        mfxEncoderDescription *enc = &desc->Enc;
        for (int codec_idx = 0; codec_idx < enc->NumCodecs; ++codec_idx) {
            auto codec = &enc->Codecs[codec_idx];
            auto codec_id = codec->CodecID;
            for (int profile_idx = 0; profile_idx < codec->NumProfiles; ++profile_idx) {
                auto profile = &codec->Profiles[profile_idx];
                for (int mem_idx = 0; mem_idx < profile->NumMemTypes; ++mem_idx) {
                    auto mem = &profile->MemDesc[mem_idx];
                    for (int fmt_idx = 0; fmt_idx < mem->NumColorFormats; ++fmt_idx) {
                        auto fmt = mem->ColorFormats[fmt_idx];
                        auto value = FromMfxCodecAndFormat(codec_id, fmt);
                        if (value != VideoCodec::UNKNOWN) {
                            auto& sup = supported_pixel_formats_[value];
                            sup.insert(fmt);

                            if (force) {
                                if (*force == value) {
                                    codecs_set.insert(value);
                                }
                            } else {
                                codecs_set.insert(value);
                            }
                        }
                        LOG_VERBOSE
                            << std::format("MFX codec '{}' profile {} supports pixel format {}",
                                           util::FourCcToString(codec_id),
                                           profile->Profile,
                                           util::FourCcToString(fmt));
                    }
                }
            }
        }
    }

    codecs->assign(codecs_set.begin(), codecs_set.end());
    std::sort(codecs->begin(), codecs->end());

    return codecs;
}

void Encoder::RunEncoder(std::stop_token st)
{
    LOG_DEBUG << "Starting video encoder thread ID " << std::this_thread::get_id();

    // Encoder initialization will start a number of background worker threads
    // when libvpl is initialized. Make sure the names of those worker threads
    // are distinct from this thread's name.
    util::SetThreadName("VMfxWorker");

    PushEvent(Event::EncoderStarting);
    if (!InitMfxEncoder()) {
        LOG_ERROR << "InitMfxEncoder() failed";
        PushEvent(Event::EncoderFailed);
        return;
    }
    PushEvent(Event::EncoderStarted);

    util::SetThreadName("VEncoderVideo");

    while (!st.stop_requested()) {
        // Get the next camera frame from the queue.
        std::shared_ptr<CameraBufferRef> cref = nullptr;
        if (params_.encoder_queue->wait_dequeue_timed(cref, 10ms)) {
            // Encode the camera frame.
            auto video_frame = EncodeCameraBuffer(*cref);

            // Get rid of this CameraFrame as soon as possible so the buffer
            // can be re-enqueued to the kernel.
            cref = nullptr;

            if (!video_frame) {
                LOG_ERROR << "EncodeCameraFrame() failed!";
                n_frames_encode_fail.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            n_frames_encode_success.fetch_add(1, std::memory_order_relaxed);

            // Enqueue the compressed video frame for network transport.
            if (params_.outgoing_video_packet_queue) {
                while (!st.stop_requested()) {
                    if (params_.outgoing_video_packet_queue->wait_enqueue_timed(video_frame, 10ms)) {
                        break;
                    } else {
                        LOG_VERBOSE << "Stalled enqueuing packet onto outgoing video packet queue, retrying";
                        n_frames_encode_stall.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }
    }

    LOG_DEBUG << "Stopping video encoder thread ID " << std::this_thread::get_id();
}

bool Encoder::InitMfxEncoder()
{
    if (!InitMfxVideoParams()) {
        LOG_ERROR << "InitMfxVideoParams() failed";
        return false;
    }

    auto status = MFXVideoVPP_Query(mfx_session_, &mfx_videoparam_vpp_, &mfx_videoparam_vpp_);
    LOG_DEBUG << "MFXVideoVPP_Query() returned: " << MfxStatusStr(status);

    status = MFXVideoVPP_Init(mfx_session_, &mfx_videoparam_vpp_);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "MFXVideoVPP_Init() failed: " << MfxStatusStr(status);
        return false;
    }

    status = MFXVideoENCODE_Query(mfx_session_, &mfx_videoparam_encode_, &mfx_videoparam_encode_);
    LOG_DEBUG << "MFXVideoENCODE_Query() returned: " << MfxStatusStr(status);

    status = MFXVideoENCODE_Init(mfx_session_, &mfx_videoparam_encode_);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "MFXVideoENCODE_Init() failed: " << MfxStatusStr(status);
        return false;
    }

    return true;
}

bool Encoder::InitMfxVideoParams()
{
    if (!SetMfxFourCc()) {
        LOG_ERROR << "SetMfxFourCc() failed";
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

    // Best quality.
    mfx_videoparam_encode_.mfx.TargetUsage = MFX_TARGETUSAGE_BEST_QUALITY;

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

    // Supposedly, maximum possible size of any compressed frames.
    mfx_videoparam_encode_.mfx.BufferSizeInKB = 256;

    // For CBR and VCM, used to estimate the targeted frame size by dividing
    // the frame rate by the bitrate.
    mfx_videoparam_encode_.mfx.TargetKbps = params_.bitrate_kbps;

    // "The maximum bitrate at which the encoded data enters the Video Buffering
    // Verifier (VBV) buffer."
    mfx_videoparam_encode_.mfx.MaxKbps = params_.bitrate_kbps;

    // Frame rate numerator.
    mfx_videoparam_vpp_.vpp.In.FrameRateExtN =
    mfx_videoparam_vpp_.vpp.Out.FrameRateExtN =
    mfx_videoparam_encode_.mfx.FrameInfo.FrameRateExtN =
        camera_format_.FrameRateN();

    // Frame rate denominator.
    mfx_videoparam_vpp_.vpp.In.FrameRateExtD =
    mfx_videoparam_vpp_.vpp.Out.FrameRateExtD =
    mfx_videoparam_encode_.mfx.FrameInfo.FrameRateExtD =
        camera_format_.FrameRateD();

    // Width of the video frame in pixels. Must be a multiple of 16.
    mfx_videoparam_vpp_.vpp.In.Width =
    mfx_videoparam_vpp_.vpp.Out.Width =
    mfx_videoparam_encode_.mfx.FrameInfo.Width =
        VACON_ALIGN16(camera_format_.Width());

    // Height of the video frame in pixels. Must be a multiple of 16.
    mfx_videoparam_vpp_.vpp.In.Height =
    mfx_videoparam_vpp_.vpp.Out.Height =
    mfx_videoparam_encode_.mfx.FrameInfo.Height =
        VACON_ALIGN16(camera_format_.Height());

    // Width in pixels.
    mfx_videoparam_vpp_.vpp.In.CropW =
    mfx_videoparam_vpp_.vpp.Out.CropW =
    mfx_videoparam_encode_.mfx.FrameInfo.CropW =
        camera_format_.Width();

    // Height in pixels.
    mfx_videoparam_vpp_.vpp.In.CropH =
    mfx_videoparam_vpp_.vpp.Out.CropH =
    mfx_videoparam_encode_.mfx.FrameInfo.CropH =
        camera_format_.Height();

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

    // Controls frame size tolerance, supposedly more strictly obeys average
    // frame size set by MaxKbps.
    mfx_eco3_.LowDelayBRC = MFX_CODINGOPTION_ON;

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
    auto it = map.find(camera_format_.FourCcStr());
    if (it != map.end()) {
        LOG_DEBUG << std::format("Using {} for video pixel format", it->first);

        // Set VPP input parameters to match the camera format.
        auto fourcc = it->second;
        fourcc.ToMfxFrameInfo(&mfx_videoparam_vpp_.vpp.In);

        // Set VPP output and encoder input parameters to a pixel format
        // suitable for 10-bit hardware encoding. Use a FourCC with the same
        // chroma format as the camera input format.
        if (fourcc.ChromaFormat == MFX_CHROMAFORMAT_YUV420) {
            LOG_DEBUG << "Using P010 for encoder pixel format";
            map["P010"].ToMfxFrameInfo(&mfx_videoparam_vpp_.vpp.Out);
            map["P010"].ToMfxFrameInfo(&mfx_videoparam_encode_.mfx.FrameInfo);
        } else if (fourcc.ChromaFormat == MFX_CHROMAFORMAT_YUV422) {
            LOG_DEBUG << "Using Y210 for encoder pixel format";
            map["Y210"].ToMfxFrameInfo(&mfx_videoparam_vpp_.vpp.Out);
            map["Y210"].ToMfxFrameInfo(&mfx_videoparam_encode_.mfx.FrameInfo);
        } else {
            LOG_ERROR << "Unhandled chroma format: " << fourcc.ChromaFormat;
            return false;
        }
    } else {
        LOG_ERROR << "Unhandled input pixel format: " << camera_format_.FourCcStr();
        return false;
    }

    // Success.
    return true;
}

std::shared_ptr<VideoFrame> Encoder::EncodeCameraBuffer(const CameraBufferRef& cref)
{
    auto t_start = std::chrono::steady_clock::now();

    // Initialize the frame's data.
    auto frame = std::make_shared<VideoFrame>(1024 * mfx_videoparam_encode_.mfx.BufferSizeInKB);
    frame->pts = cref.buf_.PtsMicros();

    // Get a new surface for storing the copy of the camera frame data.
    mfxFrameSurface1 *surface_camera = nullptr;
    auto status = MFXMemory_GetSurfaceForVPPIn(mfx_session_, &surface_camera);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "MFXMemory_GetSurfaceForVPPIn() failed: " << MfxStatusStr(status);
        return nullptr;
    }

    // Map the new surface onto the CPU for writing.
    status = surface_camera->FrameInterface->Map(surface_camera, MFX_MAP_WRITE);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "mfxFrameSurfaceInterface->Map(MFX_MAP_WRITE) failed: " << MfxStatusStr(status);
        return nullptr;
    }

    // Copy the camera frame data to the new surface.
    if (!CopyCameraBufferToSurface(cref, *surface_camera)) {
        LOG_ERROR << "Encoder::CopyCameraFrameToSurface() failed";
        return nullptr;
    }

    // Unmap the camera surface from the CPU.
    status = surface_camera->FrameInterface->Unmap(surface_camera);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "mfxFrameSurfaceInterface->Unmap() failed: " << MfxStatusStr(status);
        return nullptr;
    }

    // Issue the VPP scaling request to the GPU.
    status = MFXVideoVPP_ProcessFrameAsync(mfx_session_, surface_camera, &frame->surface);

    // Decrement reference count on the camera surface.
    status = surface_camera->FrameInterface->Release(surface_camera);
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "mfxFrameSurfaceInterface->Release() failed: " << MfxStatusStr(status);
        return nullptr;
    }

    // Check status of the scaling request.
    if (status != MFX_ERR_NONE) {
        LOG_ERROR << "MFXVideoVPP_RunFrameVPPAsync() failed: " << MfxStatusStr(status);
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
        LOG_ERROR << "MFXVideoENCODE_EncodeFrameAsync() failed: " << MfxStatusStr(status);
        return nullptr;
    }

    // Check status of the encoding request.
    if (!syncp) {
        LOG_ERROR << "MFXVideoENCODE_EncodeFrameAsync() failed to return a synchronization point";
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
        LOG_ERROR << "MFXVideoCORE_SyncOperation() failed: " << MfxStatusStr(status);
        return nullptr;
    }

    // Deallocate the uncompressed surface data.
    frame->FreeMfxSurface();

    // Stats.
    auto t_stop = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t_stop - t_start).count();
    s_encode_time_.Update(micros);
    auto msg = std::format("Encoded frame from buffer {}, sequence {} in {} us, {} bytes",
                           cref.buf_.vbuf.index,
                           cref.buf_.vbuf.sequence,
                           micros,
                           frame->CompressedDataLength());
    if (stalled) {
        LOG_DEBUG << msg;
    } else {
        LOG_VERBOSE << msg;
    }
    s_encode_size_.Update(frame->CompressedDataLength());

    // Success.
    return frame;
}

bool Encoder::CopyCameraBufferToSurface(const CameraBufferRef& cref, mfxFrameSurface1& surface)
{
    const mfxFrameInfo& info = mfx_videoparam_vpp_.vpp.In;
    auto width = info.CropW;
    auto height = info.CropH;
    auto data = cref.buf_.mmap.data();
    auto fourcc = cref.buf_.fmt.pixelformat;

    // Copy the frame info parameters from the VPP configuration.
    surface.Info = info;

    // Copy the frame data from the V4L2 mmap() buffer into the MFX surface.
    switch (fourcc) {
    case V4L2_PIX_FMT_NV12:
        memcpy(surface.Data.Y, data, width * height);
        memcpy(surface.Data.UV, data + width*height, width*height / 2);
        surface.Data.Pitch = width;
        break;

    case V4L2_PIX_FMT_YUYV:
        memcpy(surface.Data.Y, data, width*height * 2);
        surface.Data.U = surface.Data.Y + 1;
        surface.Data.V = surface.Data.Y + 3;
        surface.Data.Pitch = width * 2;
        break;

    case V4L2_PIX_FMT_UYVY:
        memcpy(surface.Data.U, data, width*height * 2);
        surface.Data.Y = surface.Data.U + 1;
        surface.Data.V = surface.Data.U + 2;
        surface.Data.Pitch = width * 2;
        break;

    default:
        LOG_ERROR << std::format("Unsupported V4L2 camera frame FourCC {} ({:#010x})",
                                 util::FourCcToString(fourcc), fourcc);
        return false;
    }

    // Success.
    return true;
}

} // namespace linux
} // namespace vacon
