#include "camera_encoder.hpp"

#include <cassert>
#include <memory>

#include <fmt/format.h>

#include "common.hpp"

namespace vacon {

std::unique_ptr<CameraEncoder> CameraEncoder::Create(const CameraEncoderParams& _params)
{
    PLOG_DEBUG <<
        fmt::format("CameraEncoderParams: device {}, codec {}, bitrate {} Kbps,"
                    " camera pixel format {}, encoder pixel format {},"
                    " width {}, height {}, frame rate {}",
                    _params.device,
                    _params.codec,
                    _params.bitrate,
                    _params.camera_pixel_format,
                    _params.encoder_pixel_format,
                    _params.width,
                    _params.height,
                    _params.frame_rate);

    auto ce = std::make_unique<CameraEncoder>(CameraEncoder {});
    ce->params = _params;

    if (!ce->initCameraDevice()) {
        PLOG_DEBUG << "initCameraDevice() failed";
        return nullptr;
    }

    if (!ce->initCodecContext()) {
        PLOG_DEBUG << "initCodecContext() failed";
        return nullptr;
    }

    if (!ce->initVaapiDevice()) {
        PLOG_DEBUG << "initVaapiDevice() failed";
        return nullptr;
    }

    return ce;
}

CameraEncoder::~CameraEncoder()
{
    PLOG_DEBUG << fmt::format("Destructor called on {}", fmt::ptr(this));
}

bool CameraEncoder::initCameraDevice()
{
    int ret;
    AVDictionary *opts = nullptr;

    // Initialize libavdevice.
    avdevice_register_all();

    // Set camera pixel format.
    if (!this->params.camera_pixel_format.empty()) {
        ret = av_dict_set(&opts, "pixel_format", this->params.camera_pixel_format.c_str(), 0);
        assert(ret >= 0);
    }

    // Set camera frame rate.
    ret = av_dict_set(&opts, "framerate", std::to_string(this->params.frame_rate).c_str(), 0);
    assert(ret >= 0);

    // Set camera frame size.
    auto video_size = fmt::format("{}x{}", this->params.width, this->params.height);
    ret = av_dict_set(&opts, "video_size", video_size.c_str(), 0);
    assert(ret >= 0);

    // Find the V4L2 input format.
    const AVInputFormat *ifmt = av_find_input_format("video4linux2");
    if (!ifmt) {
        PLOG_ERROR << "av_find_input_format() failed to find input format video4linux2";
        return false;
    }

    // Open V4L2 input device and allocate AVFormatContext.
    ret = avformat_open_input(&this->fmt_ctx, this->params.device.c_str(), ifmt, &opts);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("avformat_open_input() failed on v4l2 device {}: {}",
                                  this->params.device, av_err2str(ret));
        return false;
    }

    // Latency hacks.
    this->fmt_ctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

    // Retrieve stream information. Starts to read data from the media stream.
    ret = avformat_find_stream_info(this->fmt_ctx, nullptr);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("avformat_find_stream_info failed on v4l2 device {}: {}",
                                  this->params.device, av_err2str(ret));
        return false;
    }

    // Log information about the newly opened input.
    av_dump_format(this->fmt_ctx, 0, this->params.device.c_str(), 0);

    // Success.
    return true;
}

bool CameraEncoder::initCodecContext()
{
    // Find the index of the video stream inside the AVFormatContext.
    int ret = av_find_best_stream(this->fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("Could not find video stream in input on v4l2 device {}",
                                  this->params.device);
        return false;
    }
    this->video_stream_idx = ret;

    // Find the decoder for the video stream.
    AVStream *st = this->fmt_ctx->streams[this->video_stream_idx];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        PLOG_ERROR << "Failed to find video decoder";
        return false;
    }

    // Allocate AVCodecContext and initialize to default values.
    this->dec_ctx = avcodec_alloc_context3(dec);
    if (!this->dec_ctx) {
        PLOG_ERROR << "Failed to allocate video decoder context";
        return false;
    }

    // Copy codec parameters from input stream to output codec context.
    ret = avcodec_parameters_to_context(this->dec_ctx, st->codecpar);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("Failed to copy video codec parameters to decoder context: {}",
                                  av_err2str(ret));
        return false;
    }

    // Latency hack.
    this->dec_ctx->thread_count = 1;

    // Initialize the decoder.
    ret = avcodec_open2(this->dec_ctx, dec, nullptr);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("Failed to open video decoder: {}", av_err2str(ret));
        return false;
    }

    // Success.
    return true;
}

bool CameraEncoder::initVaapiDevice()
{
    // Create the VAAPI device for hardware video encoding.
    int ret = av_hwdevice_ctx_create(&this->hw_device_ctx,
                                     AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("Failed to create VAAPI device: {}", av_err2str(ret));
        return false;
    }

    // Look up the encoder registered for the hardware codec.
    const AVCodec *hw_codec = avcodec_find_encoder_by_name(this->params.codec.c_str());
    if (!hw_codec) {
        PLOG_ERROR << fmt::format("Could not find encoder for codec '{}'", this->params.codec);
        return false;
    }

    // Initialize the AVCodecContext for the hardware video encoder. Only fails
    // due to memory allocation.
    this->hw_ctx = avcodec_alloc_context3(hw_codec);
    assert(this->hw_ctx);

    // Configure the AVCodecContext for the hardware video encoder.
    hw_ctx->sample_aspect_ratio = { 1, 1 };
    hw_ctx->width               = this->dec_ctx->width;
    hw_ctx->height              = this->dec_ctx->height;
    hw_ctx->framerate           = { this->params.frame_rate, 1 };
    hw_ctx->time_base           = { 1, this->params.frame_rate };
    hw_ctx->pix_fmt             = AV_PIX_FMT_VAAPI;
    hw_ctx->max_b_frames        = 0;
    hw_ctx->gop_size            = 60;
    hw_ctx->bit_rate            = this->params.bitrate * 1000;
    hw_ctx->rc_max_rate         = this->params.bitrate * 1000;

    // Latency hacks.
    hw_ctx->thread_count = 1;
    if (av_opt_set(hw_ctx->priv_data, "async_depth", "1", 0) != 0) {
        PLOG_WARNING << "Failed to set async_depth = 1 on VAAPI AVCodecContext!";
    }

    // Initialize the hardware frame pool.
    if (!initVaapiHwFramePool()) {
        return false;
    }

    // Initialize the AVCodecContext using the AVCodec.
    ret = avcodec_open2(this->hw_ctx, hw_codec, nullptr);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("Cannot open hardware video encoder: {}", av_err2str(ret));
        return false;
    }

    // Initialize video data storage.
    this->frame = av_frame_alloc();
    this->pkt = av_packet_alloc();
    assert(this->frame);
    assert(this->pkt);

    // Success.
    return true;
}

bool CameraEncoder::initVaapiHwFramePool()
{
    AVBufferRef *hw_frames_ref = av_hwframe_ctx_alloc(this->hw_device_ctx);
    if (!hw_frames_ref) {
        PLOG_ERROR << "Failed to create VAAPI hardware frame context!";
        return false;
    }
    AVHWFramesContext *frames_ctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ref->data);

    // Configure the hardware frame pool.
    frames_ctx->initial_pool_size   = 20;
    frames_ctx->width               = this->dec_ctx->width;
    frames_ctx->height              = this->dec_ctx->height;
    frames_ctx->format              = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format           = av_get_pix_fmt(this->params.encoder_pixel_format.c_str());

    // Pick a good default if the hardware surface format wasn't specified.
    if (frames_ctx->sw_format == AV_PIX_FMT_NONE) {
        // P010 requires a hardware encoder that supports 10-bit color.
        frames_ctx->sw_format = AV_PIX_FMT_P010;
    }

    // Initialize hardware frame pool.
    int ret = av_hwframe_ctx_init(hw_frames_ref);
    if (ret < 0) {
        PLOG_ERROR << fmt::format("Failed to initialize VAAPI hardware frame context: {}",
                                  av_err2str(ret));
        av_buffer_unref(&hw_frames_ref);
        return false;
    }
    this->hw_ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    assert(this->hw_ctx->hw_frames_ctx);

    // Cleanup.
    av_buffer_unref(&hw_frames_ref);

    // Success.
    return true;
}

bool CameraEncoder::encodePackets(std::stop_token st,
                                  std::function<void(const AVPacket*)> callback)
{
    while (!st.stop_requested()) {
    }

    // Success.
    return true;
}

} // namespace vacon