#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>
#include <readerwriterqueue.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

using namespace std::chrono_literals;

using std::byte;
using std::make_shared;
using std::shared_ptr;
using std::string;
using std::weak_ptr;

using moodycamel::BlockingReaderWriterQueue;
using nlohmann::json;

#ifdef av_err2str
#undef av_err2str
av_always_inline std::string av_err2string(int errnum) {
	char str[AV_ERROR_MAX_STRING_SIZE];
	return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#define av_err2str(err) av_err2string(err).c_str()
#endif

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

static BlockingReaderWriterQueue<rtc::binary> g_rtp_packet_queue(100);

static shared_ptr<rtc::PeerConnection> g_pc;
static shared_ptr<rtc::WebSocket> g_ws;

static AVFormatContext *rtp_fctx;
static unsigned char *rtp_buf;
static AVIOContext *rtp_ioctx;
static int rtp_video_stream;

static AVBufferRef *hw_device_ctx = NULL;
static AVCodecContext *decoder_ctx = NULL;

static enum AVPixelFormat getVaapiFormat(AVCodecContext *ctx __attribute__((unused)),
					 const enum AVPixelFormat *pix_fmts)
{
	for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == AV_PIX_FMT_VAAPI) {
			return *p;
		}
	}

	std::cerr << "Unable to decode this content using VA-API." << std::endl;
	return AV_PIX_FMT_NONE;
}

static int readAvioPacketRTP(void *ptr __attribute__((unused)),
			     uint8_t *buf,
			     int buf_size)
{
	rtc::binary message;
	long count_rtp_packets = 0;
	long count_rtp_bytes = 0;

	g_rtp_packet_queue.wait_dequeue(message);

	assert(message.size() > 0);
	assert((int)message.size() <= buf_size);

	count_rtp_packets += 1;
	count_rtp_bytes += message.size();
	if ((count_rtp_packets % 10000) == 0) {
		std::cout
			<< "[runFfmpeg] Received "
			<< count_rtp_packets
			<< " packets, "
			<< count_rtp_bytes
			<< " bytes from RTP peer, queue size "
			<< g_rtp_packet_queue.size_approx()
			<< ", max queue capacity "
			<< g_rtp_packet_queue.max_capacity()
			<< std::endl;
	}

	memcpy(buf, message.data(), message.size());

	return message.size();
}

static int writeAvioPacketRTP(void *ptr __attribute__((unused)),
			      /* const */ uint8_t *buf __attribute__((unused)),
			      int buf_size __attribute__((unused)))
{
	//std::cerr << "[writeAvioPacketRTP] Want to write " << buf_size << " bytes " << std::endl;
	return buf_size;
}

static int readAvioPacketString(void *ptr, uint8_t *buf, int buf_size)
{
	string &s = *(static_cast<string*>(ptr));
	buf_size = FFMIN(buf_size, s.length());
	const char *data = s.c_str();

	// Don't copy the NUL at the end of the string.
	if (buf_size >= 1 && data[buf_size - 1] == '\0') {
		buf_size -= 1;
	}

	if (!buf_size) {
		return AVERROR_EOF;
	}

	memcpy(buf, data, buf_size);
	s.erase(0, buf_size);

	return buf_size;
}

static bool setupFfmpeg(void)
{
	int ret;

	// Fake SDP descriptor.
	// XXX: Generate this dynamically.
	string sdp_string(
		"c=IN IP4 127.0.0.1\n"
		"m=video 5000 RTP/AVP 96\n"
		"a=rtpmap:96 H265/90000\n"
	);

	// Create format options dictionary for the SDP input format, set
	// RTSP_FLAG_CUSTOM_IO.
	AVDictionary *sdp_options = NULL;
	ret = av_dict_set(&sdp_options, "sdp_flags", "custom_io", 0);
	assert(ret >= 0);

	// Find AVInputFormat* for the SDP input format.
	const AVInputFormat *sdp_iformat = av_find_input_format("sdp");
	assert(sdp_iformat != NULL);

	// Allocate AVFormatContext* for the RTP stream.
	rtp_fctx = avformat_alloc_context();
	assert(rtp_fctx != NULL);

	// Allocate fixed size buffer for readAvioPacketString() to write into.
	const size_t sdp_ioctx_buf_size = 1024;
	unsigned char *sdp_ioctx_buf = (unsigned char *)av_malloc(sdp_ioctx_buf_size);
	assert(sdp_ioctx_buf != NULL);

	// Create AVIOContext for reading the fake session descriptor from an
	// in-memory buffer. Must be freed with avio_context_free().
	AVIOContext *sdp_ioctx =
		avio_alloc_context(sdp_ioctx_buf,			// buffer
				   sdp_ioctx_buf_size,			// buffer_size
				   0,					// write_flag
				   static_cast<void*>(&sdp_string),	// opaque
				   readAvioPacketString,		// read_packet
				   NULL,				// write_packet
				   NULL);				// seek
	assert(sdp_ioctx != NULL);

	// Plug in the custom AVIOContext for reading the fake session
	// descriptor to the AVFormatContext.
	rtp_fctx->pb = sdp_ioctx;

	// Open the RTP input stream. This will read the fake in-memory session
	// descriptor. Must be closed with avformat_close_input().
	ret = avformat_open_input(&rtp_fctx, NULL, sdp_iformat, &sdp_options);
	assert(ret == 0);

	// Now that the fake in-memory SDP has been completely read by
	// avformat_open_input(), swap out the AVFormatContext's I/O context
	// for a new AVIOContext that reads packets from the RTP stream.

	// Allocate fixed size buffer for readAvioPacketRTP() to write into.
	rtp_buf = (unsigned char *)av_malloc(1500);
	assert(rtp_buf != NULL);

	// Create AVIOContext for reading RTP packets from a callback. Must be
	// freed with avio_context_free().
	rtp_ioctx =
		avio_alloc_context(rtp_buf,		// buffer
				   1500,		// buffer size
				   1,			// write_flag
				   NULL,		// opaque
				   readAvioPacketRTP,	// read_packet
				   writeAvioPacketRTP,	// write_packet
				   NULL);		// seek
	assert(rtp_ioctx != NULL);

	// Plug in the custom AVIOContext for reading RTP packets to the
	// AVFormatContext.
	rtp_fctx->pb = rtp_ioctx;

	// Read packets of a media file to get stream information. The logical
	// file position is not changed by this function; examined packets may
	// be buffered for later processing.
	if ((ret = avformat_find_stream_info(rtp_fctx, NULL)) < 0) {
		std::cerr
			<< "Cannot find input stream information. Error code: "
			<< av_err2str(ret)
			<< std::endl;
		return false;
	}

	// ???
	const AVCodec *decoder = NULL;
	ret = av_find_best_stream(rtp_fctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
	if (ret < 0) {
		std::cerr
			<< "Cannot find a video stream in the input. Error code: "
			<< ret
			<< std::endl;
		return false;
	}
	// ???
	rtp_video_stream = ret;

	// Log detailed information about the video stream.
	av_dump_format(rtp_fctx, rtp_video_stream, "(custom in-memory I/O)", 0 /* is_output */);

	// Allocate an AVCodecContext and set its fields to default values. The
	// resulting struct should be freed with avcodec_free_context().
	if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
		std::cerr
			<< "avcodec_alloc_context3() failed"
			<< std::endl;
		return false;
	}

	// Fill the codec context based on the values from the supplied codec
	// parameters. Any allocated fields in `codec` (decoder_ctx) that have
	// a corresponding field in `par` (video->codecpar) are freed and
	// replaced with duplicates of the corresponding field in `par`. Fields
	// in `codec` that do not have a counterpart in par are not touched.
	AVStream *video = rtp_fctx->streams[rtp_video_stream];
	if ((ret = avcodec_parameters_to_context(decoder_ctx, video->codecpar)) < 0) {
		std::cerr
			<< "avcodec_parameters_to_context() failed with error code "
			<< ret
			<< std::endl;
		return false;
	}

	// Open the VAAPI device and create the AVHWDeviceContext.
	ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
	if (ret < 0) {
		std::cerr
			<< "Failed to open the VAAPI device."
			<< " av_hwdevice_ctx_create() failed with error code "
			<< ret
			<< std::endl;
		return false;
	}

	// ???
	decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
	if (!decoder_ctx->hw_device_ctx) {
		std::cerr << "A hardware device reference create failed" << std::endl;
		return false;
	}
	// ???
	decoder_ctx->get_format = getVaapiFormat;

	// Initialize the AVCodecContext (decoder_ctx) to use the given AVCodec
	// (decoder). The AVCodecContext must have been allocated with
	// avcodec_alloc_context3().
	if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
		std::cerr
			<< "Failed to open codec for decoding. Error code: "
			<< ret
			<< std::endl;
		return false;
	}

	return true;
}

static int decodePacket(AVPacket *pkt)
{
	int ret = 0;

	// Supply raw packet data as input to the decoder. Internally, this
	// call will copy relevant AVCodecContext fields, which can influence
	// decoding per-packet, and apply them when the packet is actually
	// decoded. (For example AVCodecContext.skip_frame, which might direct
	// the decoder to drop the frame contained by the packet sent with this
	// function.)
	ret = avcodec_send_packet(decoder_ctx, pkt);
	if (ret < 0) {
		std::cerr
			<< "[decodePacket] Error during decoding (ignoring). Error code: "
			<< av_err2str(ret)
			<< std::endl;
		//return ret;
		return 0;
	}

	static long count_bytes_processed = 0;
	static long count_frames_processed = 0;

	while (ret >= 0) {
		// AVFrame for holding the output from the decoder.
		AVFrame *frame;

		// Allocate the AVFrame and set its fields to default values.
		// Must be freed using av_frame_free(). This only allocates the
		// AVFrame itself, not the data buffers.
		if (!(frame = av_frame_alloc())) {
			std::cerr << "[decodePacket] av_frame_alloc() failed" << std::endl;
			return AVERROR(ENOMEM);
		}

		// Return decoded output data from the decoder into `frame`,
		// which will contain refcounted video frame data allocated by
		// the codec.
		ret = avcodec_receive_frame(decoder_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			// EAGAIN: Output is not available in this state.
			// Caller must try to send new input.
			//
			// EOF: Codec has been fully flushed, and there will be
			// no more output frames.
			av_frame_free(&frame);
			return 0;
		} else if (ret < 0) {
			std::cerr
				<< "[decodePacket] Error while decoding. Error code: "
				<< av_err2str(ret)
				<< std::endl;
			goto fail;
		}


		count_bytes_processed += pkt->size;
		count_frames_processed += 1;

		if ((count_frames_processed % 60) == 0) {
			std::cerr
				<< "[decodePacket] Processed "
				<< count_frames_processed
				<< " frames, "
				<< count_bytes_processed
				<< " bytes"
				<< std::endl;
		}

fail:
		// Clean up from av_frame_alloc() above.
		av_frame_free(&frame);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static void runFfmpeg(void)
{
	int ret = 0;

	// Allocate an AVPacket and set its fields to default values.
	// Must be freed with av_packet_free().
	AVPacket *dec_pkt = av_packet_alloc();
	if (!dec_pkt) {
		std::cerr << "Failed to allocate decode packet" << std::endl;
		return;
	}

	int count_frames_recv = 0;
	while (ret >= 0) {
		if ((ret = av_read_frame(rtp_fctx, dec_pkt)) < 0) {
			break;
		}

		if (rtp_video_stream != dec_pkt->stream_index) {
			continue;
		}

		count_frames_recv += 1;
		if ((count_frames_recv % 600) == 0) {
			std::cerr
				<< "[runFfmpeg] Received "
				<< count_frames_recv
				<< " frames"
				<< std::endl;
		}

		ret = decodePacket(dec_pkt);

		av_packet_unref(dec_pkt);
	}

	std::cerr << "[runFfmpeg] Shutting down: " << av_err2str(ret) << std::endl;
}

static void createPeerConnection(const rtc::Configuration &config,
				 const rtc::Description &offer,
				 weak_ptr<rtc::WebSocket> wws)
{
	g_pc = make_shared<rtc::PeerConnection>(config);

	g_pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "[PeerConnection onStateChange] State: " << state << std::endl;
	});

	g_pc->onGatheringStateChange([wws](rtc::PeerConnection::GatheringState state) {
		std::cout << "[PeerConnection onGatheringStateChange] State: " << state << std::endl;
		if (state == rtc::PeerConnection::GatheringState::Complete) {
			std::cout << "[PeerConnection onGatheringStateChange] Local Description: " << std::endl;
			auto description = g_pc->localDescription();
			json message = {
				{ "id", "server" },
				{ "type", description->typeString() },
				{ "sdp", std::string(description.value()) },
			};
			auto message_dump = message.dump();
			std::cout << "[PeerConnection] Sending WebSocket message: "
				<< message_dump
				<< std::endl;
			if (auto ws = wws.lock()) {
				ws->send(message_dump);
				ws->close();
			}
		}
	});

	g_pc->onLocalDescription([](rtc::Description description) {
		std::cout << "[PeerConnection onLocalDescription] Local Description: " << std::endl;
		json message = {
			{ "type", description.typeString() },
			{ "sdp", std::string(description) },
		};
		std::cout << message << std::endl;
	});

	rtc::Description::Video video("video", rtc::Description::Direction::RecvOnly);
	video.addH265Codec(96);

	auto track = g_pc->addTrack(video);

	auto session = make_shared<rtc::RtcpReceivingSession>();
	track->setMediaHandler(session);

	track->onMessage([track](rtc::binary message) {
		// This is an RTP packet.
		g_rtp_packet_queue.emplace(message);
	    },
	    nullptr);

	g_pc->setRemoteDescription(offer);
}

static void wsOnMessage(json message,
			rtc::Configuration config,
			shared_ptr<rtc::WebSocket> ws)
{
	std::cout << "[wsOnMessage] Received WebSocket message: " << message << std::endl;

	if (!message.contains("id")) {
		std::cout << "Message lacks key 'id'" << std::endl;
		return;
	}
	auto id = message.find("id")->get<string>();

	if (!message.contains("type")) {
		std::cout << "Message lacks key 'type'" << std::endl;
		return;
	}
	auto type = message.find("type")->get<string>();

	if (type == "offer") {
		auto sdp = message["sdp"].get<string>();
		auto description = rtc::Description(sdp, type);
		createPeerConnection(config, description, make_weak_ptr(ws));

		if (setupFfmpeg()) {
			std::jthread thr_ffmpeg_worker(runFfmpeg);
		} else {
			std::cerr << "Couldn't setup ffmpeg libraries!" << std::endl;
		}
	}
}

int video_receiver(const string& ws_url)
{
	rtc::InitLogger(rtc::LogLevel::Debug);

	rtc::Configuration config;
	//string stunServer = "stun:stun.l.google.com:19302";
	//config.iceServers.emplace_back(stunServer);
	//config.disableAutoNegotiation = true;

	auto ws = make_shared<rtc::WebSocket>();

	std::promise<void> wsPromise;
	auto wsFuture = wsPromise.get_future();

	ws->onOpen([&wsPromise]() {
		std::cout << "WebSocket connected, signaling ready" << std::endl;
		wsPromise.set_value();
	});

	ws->onClosed([]() {
		std::cout << "WebSocket closed" << std::endl;
	});

	ws->onError([&wsPromise](std::string s) {
		std::cout << "WebSocket error" << std::endl;
		wsPromise.set_exception(std::make_exception_ptr(std::runtime_error(s)));
	});

	ws->onMessage([&](std::variant<rtc::binary, rtc::string> data) {
		if (std::holds_alternative<rtc::string>(data)) {
			json message = json::parse(std::get<rtc::string>(data));
			wsOnMessage(message, config, ws);
		}
	});

	const string url = ws_url + "/client";
	std::cout << "Opening WebSocket URL " << url << std::endl;
	ws->open(url);

	std::cout << "Waiting for signaling to be connected..." << std::endl;
	wsFuture.get();

	json message = {
		{ "id", "server" },
		{ "type", "request" },
	};
	auto message_dump = message.dump();
	std::cout << "Sending WebSocket message: " << message_dump << std::endl;
	ws->send(message_dump);

	std::cout << "Press any key to exit." << std::endl;
	char dummy;
	std::cin >> dummy;

	return 0;
}
