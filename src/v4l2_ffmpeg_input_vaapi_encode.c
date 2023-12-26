/*
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file Intel VAAPI-accelerated encoding API usage example
 * @example vaapi_encode.c
 *
 * Perform VAAPI-accelerated encoding. Read input from an NV12 raw
 * file, and write the H.264 encoded data to an output raw file.
 * Usage: vaapi_encode 1920 1080 input.yuv output.h264
 */

#include <bsd/sys/time.h>
#include <sys/time.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <locale.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>

static clockid_t clockid = CLOCK_MONOTONIC_RAW;
static int video_frame_count;
static int count_frames_sent;
static int count_packets_recv;
static int bytes_packets_recv;

static int width, height;
static AVBufferRef *hw_device_ctx = NULL;

// Things to do with the output.
static AVCodecContext *avctx = NULL;

// Things to do with the v4l2 input.
static int video_stream_idx = -1;
static int width, height;
static enum AVPixelFormat pix_fmt;
static AVCodecContext *video_dec_ctx = NULL;
static AVFormatContext *fmt_ctx = NULL;
static AVFrame *frame = NULL;
static AVPacket *pkt = NULL;

static AVFrame *sw_frame = NULL, *hw_frame = NULL;
static const AVCodec *codec = NULL;

static void (*encode_write_callback)(AVPacket *);

static int open_codec_context(int *stream_idx, AVCodecContext **dec_ctx,
			      AVFormatContext *av_fmt_ctx, enum AVMediaType type)
{
	int ret = av_find_best_stream(av_fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0) {
		fprintf(stderr, "Could not find %s stream in input\n",
			av_get_media_type_string(type));
		return ret;
	} else {
		int stream_index = ret;
		AVStream *st = av_fmt_ctx->streams[stream_index];

		/* find decoder for the stream */
		const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
		if (!dec) {
			fprintf(stderr, "Failed to find %s codec\n",
				av_get_media_type_string(type));
			return AVERROR(EINVAL);
		}

		/* Allocate a codec context for the decoder */
		*dec_ctx = avcodec_alloc_context3(dec);
		if (!*dec_ctx) {
			fprintf(stderr,
				"Failed to allocate the %s codec context\n",
				av_get_media_type_string(type));
			return AVERROR(ENOMEM);
		}

		/* Copy codec parameters from input stream to output codec context */
		if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
			fprintf(stderr,
				"Failed to copy %s codec parameters to decoder context\n",
				av_get_media_type_string(type));
			return ret;
		}

		/* Other settings */
		(*dec_ctx)->thread_count = 1;

		/* Init the decoders */
		if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
			fprintf(stderr, "Failed to open %s codec\n",
				av_get_media_type_string(type));
			return ret;
		}
		*stream_idx = stream_index;
	}

	return 0;
}

static long timespec_to_ns(const struct timespec *ts)
{
	return (ts->tv_sec * 1000000000) + (ts->tv_nsec);
}

static int set_hwframe_ctx(AVCodecContext *ctx, AVBufferRef *hw_device_ctx)
{
	AVBufferRef *hw_frames_ref;
	AVHWFramesContext *frames_ctx = NULL;
	int err = 0;
	char *env;

	if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
		fprintf(stderr, "Failed to create VAAPI frame context.\n");
		return -1;
	}
	frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
	frames_ctx->format = AV_PIX_FMT_VAAPI;
	frames_ctx->width = width;
	frames_ctx->height = height;
	frames_ctx->initial_pool_size = 20;

	enum AVPixelFormat format = AV_PIX_FMT_NONE;

	if ((env = getenv("VACON_HW_PIXEL_FORMAT"))) {
		if (strcasecmp(env, "nv12") == 0) {
			format = AV_PIX_FMT_NV12;
		}
	}

	if (format == AV_PIX_FMT_NONE) {
		format = AV_PIX_FMT_P010;
	}

	frames_ctx->sw_format = format;

	if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
		fprintf(stderr,
			"Failed to initialize VAAPI frame context."
			"Error code: %s\n",
			av_err2str(err));
		av_buffer_unref(&hw_frames_ref);
		return err;
	}
	ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
	if (!ctx->hw_frames_ctx)
		err = AVERROR(ENOMEM);

	av_buffer_unref(&hw_frames_ref);
	return err;
}

static int encode_write(AVCodecContext *avctx, AVFrame *frame)
{
	struct timespec t_send_a, t_send_b, t_send;
	struct timespec t_recv_a, t_recv_b, t_recv;
	struct timespec t_write_a, t_write_b, t_write;
	static long acc_sent_ns, acc_recv_ns, acc_write_ns;

	int ret = 0;
	AVPacket *enc_pkt;

	// Initialize a new packet.
	if (!(enc_pkt = av_packet_alloc()))
		return AVERROR(ENOMEM);

	// Send the raw video frame to the encoder.
	clock_gettime(clockid, &t_send_a);
	if ((ret = avcodec_send_frame(avctx, frame)) < 0) {
		fprintf(stderr, "avcodec_send_frame() failed: %s\n", av_err2str(ret));
		goto end;
	}
	clock_gettime(clockid, &t_send_b);
	timespecsub(&t_send_b, &t_send_a, &t_send);
	acc_sent_ns += timespec_to_ns(&t_send);

	count_frames_sent += 1;

	for (;;) {
		clock_gettime(clockid, &t_recv_a);
		ret = avcodec_receive_packet(avctx, enc_pkt);
		if (ret) {
			break;
		}
		clock_gettime(clockid, &t_recv_b);
		timespecsub(&t_recv_b, &t_recv_a, &t_recv);
		acc_recv_ns += timespec_to_ns(&t_recv);

		count_packets_recv += 1;

		// ???
		enc_pkt->stream_index = 0;

		// Write encoded packet using callback.
		if (encode_write_callback) {
			clock_gettime(clockid, &t_write_a);
			encode_write_callback(enc_pkt);
			clock_gettime(clockid, &t_write_b);
			timespecsub(&t_write_b, &t_write_a, &t_write);
			acc_write_ns += timespec_to_ns(&t_write);
		}

		bytes_packets_recv += enc_pkt->size;
		static int every_n_packets = 600;
		if (count_packets_recv == every_n_packets) {
			fprintf(stderr,
				"Sent %d frames"
				", got %d packets"
				", %.2f Mbits"
				", %'d bytes/packet"
				", latencies:"
				" %'ld us frame send"
				", %'ld us packet receive"
				", %'ld us packet write"
				"\n",
				count_frames_sent,
				count_packets_recv,
				bytes_packets_recv / 125000.0,
				bytes_packets_recv / every_n_packets,
				acc_sent_ns / every_n_packets / 1000,
				acc_recv_ns / every_n_packets / 1000,
				acc_write_ns / every_n_packets / 1000
			);
			count_frames_sent = 0;
			count_packets_recv = 0;
			bytes_packets_recv = 0;
			acc_sent_ns = 0;
			acc_recv_ns = 0;
			acc_write_ns = 0;
		}

		// Cleanup.
		av_packet_unref(enc_pkt);
	}

end:
	av_packet_free(&enc_pkt);
	ret = ((ret == AVERROR(EAGAIN)) ? 0 : -1);
	return ret;
}

static int output_video_frame(AVFrame *sw_frame)
{
	int err;

	video_frame_count += 1;

	if (sw_frame->width != width || sw_frame->height != height || sw_frame->format != pix_fmt) {
		/* To handle this change, one could call av_image_alloc again and
		 * decode the following frames into another rawvideo file. */
		fprintf(stderr,
			"Error: Width, height and pixel format have to be "
			"constant in a rawvideo file, but the width, height or "
			"pixel format of the input video changed:\n"
			"old: width = %d, height = %d, format = %s\n"
			"new: width = %d, height = %d, format = %s\n",
			width, height, av_get_pix_fmt_name(pix_fmt),
			sw_frame->width, sw_frame->height,
			av_get_pix_fmt_name(sw_frame->format));
		return -1;
	}

	// Initialize new frame for storing hardware frame data.
	AVFrame *hw_frame = av_frame_alloc();
	if (hw_frame == NULL) {
		fprintf(stderr, "av_frame_alloc() failed\n");
		return -1;
	}

	// Fill `hw_frame` with newly allocated buffers attached to the AVHWFramesContext.
	if ((err = av_hwframe_get_buffer(avctx->hw_frames_ctx, hw_frame, 0)) < 0) {
		fprintf(stderr, "av_hwframe_get_buffer() failed: %s\n", av_err2str(err));
		return -1;
	}
	if (hw_frame->hw_frames_ctx == NULL) {
		fprintf(stderr, ":-(\n");
		return -1;
	}

	// Copy data from the software frame to the hardware frame.
	if ((err = av_hwframe_transfer_data(hw_frame /* dst */,
					    sw_frame /* src */,
					    0 /* flags */)) < 0)
	{
		fprintf(stderr, "av_hwframe_transfer_data() failed: %s\n", av_err2str(err));
		return -1;
	}
	hw_frame->pts = sw_frame->pts;

	if (encode_write(avctx, hw_frame) < 0) {
		fprintf(stderr, "encode_write() failed\n");
		return -1;
	}

	// Cleanup.
	av_frame_free(&hw_frame);

	return 0;
}

static int decode_packet(AVCodecContext *dec, const AVPacket *pkt)
{
	int ret = 0;

	// Submit the packet to the decoder.
	ret = avcodec_send_packet(dec, pkt);
	if (ret < 0) {
		fprintf(stderr, "Error submitting a packet for decoding (%s)\n", av_err2str(ret));
		return ret;
	}

	// Process all the available frames from the decoder.
	while (ret >= 0) {
		ret = avcodec_receive_frame(dec, frame);
		if (ret < 0) {
			// These two return values are special and mean there is no output
			// frame available, but there were no errors during decoding.
			if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) {
				return 0;
			}

			fprintf(stderr, "Error during decoding (%s)\n", av_err2str(ret));
			return ret;
		}

		// Write the frame data to the output.
		if (dec->codec->type == AVMEDIA_TYPE_VIDEO) {
			ret = output_video_frame(frame);
		}

		av_frame_unref(frame);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static bool setup_input(const char *device)
{
	avdevice_register_all();

	const AVInputFormat *ifmt = av_find_input_format("video4linux2");
	if (ifmt == NULL) {
		av_log(0, AV_LOG_ERROR, "Cannot find input format\n");
		exit(1);
	}

	AVDictionary *options = NULL;

	int ret;
	char *env;

	if ((env = getenv("VACON_V4L2_FRAMERATE"))) {
		ret = av_dict_set(&options, "framerate", env, 0);
	} else {
		ret = av_dict_set(&options, "framerate", "60", 0);
	}
	assert(ret >= 0);

	if ((env = getenv("VACON_V4L2_VIDEO_SIZE"))) {
		ret = av_dict_set(&options, "video_size", env, 0);
	} else {
		ret = av_dict_set(&options, "video_size", "1920x1080", 0);
	}
	assert(ret >= 0);

	if ((env = getenv("VACON_V4L2_PIXEL_FORMAT"))) {
		ret = av_dict_set(&options, "pixel_format", env, 0);
	} else {
		ret = av_dict_set(&options, "pixel_format", "yuyv422", 0);
	}
	assert(ret >= 0);

	// Open input device and allocate format context.
	if (avformat_open_input(&fmt_ctx, device, ifmt, &options) < 0) {
		fprintf(stderr, "Could not open device %s\n", device);
		return false;
	}

	// Latency hacks.
	fmt_ctx->flags = AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

	// Retrieve stream information.
	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
		fprintf(stderr, "Could not find stream information\n");
		return false;
	}

	if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx,
			       AVMEDIA_TYPE_VIDEO) < 0)
	{
		fprintf(stderr, "Could not find video stream in the input, aborting\n");
		return false;
	}

	width = video_dec_ctx->width;
	height = video_dec_ctx->height;
	pix_fmt = video_dec_ctx->pix_fmt;

	// Dump input information to stderr
	av_dump_format(fmt_ctx, 0, device, 0);

	return true;
}

int start_v4l2_vaapi_encoder(const char *device, void (*cb)(AVPacket *))
{
	setlocale(LC_ALL, "");

	encode_write_callback = cb;

	int err;

	if (!setup_input(device)) {
		return -1;
	}

	// Create the VAAPI device for hardware video encoding.
	err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
	if (err < 0) {
		fprintf(stderr, "Failed to create a VAAPI device. Error code: %s\n", av_err2str(err));
		goto close;
	}

	// Look up the registered encoder.
	if (!(codec = avcodec_find_encoder_by_name("hevc_vaapi"))) {
		fprintf(stderr, "Could not find encoder.\n");
		err = -1;
		goto close;
	}

	// Initialize the codec context `avctx` for the hardware video encoder.
	if (!(avctx = avcodec_alloc_context3(codec))) {
		err = AVERROR(ENOMEM);
		goto close;
	}

	// Configure the codec context for the hardware video encoder.
	avctx->thread_count = 1;
	avctx->width = width;
	avctx->height = height;
	avctx->time_base = (AVRational){ 1, 60 };
	avctx->framerate = (AVRational){ 60, 1 };
	avctx->sample_aspect_ratio = (AVRational){ 1, 1 };
	avctx->pix_fmt = AV_PIX_FMT_VAAPI;
	avctx->max_b_frames = 0;
	avctx->gop_size = 60;
	avctx->bit_rate = 12 * 1000 * 1000;
	avctx->rc_max_rate = 12 * 1000 * 1000;

	if (av_opt_set(avctx->priv_data, "async_depth", "1", 0) != 0) {
		fprintf(stderr, "%s: setting async_depth failed\n", __func__);
	}

	/* set hw_frames_ctx for encoder's AVCodecContext */
	if ((err = set_hwframe_ctx(avctx, hw_device_ctx)) < 0) {
		fprintf(stderr, "Failed to set hwframe context.\n");
		goto close;
	}

	// Initialize the AVCodecContext `avctx` to use the AVCodec `codec`.
	if ((err = avcodec_open2(avctx, codec, NULL)) < 0) {
		fprintf(stderr,
			"Cannot open video encoder codec. Error code: %s\n",
			av_err2str(err));
		goto close;
	}

	// Create an AVFrame for ???
	frame = av_frame_alloc();
	if (!frame) {
		fprintf(stderr, "Could not allocate frame\n");
		goto close;
	}

	// Create an AVPacket for ???
	pkt = av_packet_alloc();
	if (!pkt) {
		fprintf(stderr, "Could not allocate packet\n");
		goto close;
	}

	// Read video frames from the input.
	while (av_read_frame(fmt_ctx, pkt) >= 0) {
		if (pkt->size <= 0 || pkt->stream_index != video_stream_idx) {
			continue;
		}

		int ret = decode_packet(video_dec_ctx, pkt);

		// Cleanup.
		av_packet_unref(pkt);

		if (ret < 0) {
			break;
		}
	}

	// Flush the decoder.
	if (video_dec_ctx) {
		decode_packet(video_dec_ctx, NULL);
	}

	// Flush the encoder.
	err = encode_write(avctx, NULL);
	if (err == AVERROR_EOF) {
		err = 0;
	}

close:
	av_frame_free(&sw_frame);
	av_frame_free(&hw_frame);
	avcodec_free_context(&avctx);
	av_buffer_unref(&hw_device_ctx);
	return err;
}
