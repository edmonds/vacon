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

extern "C" {
int plplay_play(AVFormatContext *);
}

static int readAvioPacketRTP(void *ptr __attribute__((unused)),
			     uint8_t *buf,
			     int buf_size)
{
	rtc::binary message;
	static long count_rtp_packets = 0;
	static long count_rtp_bytes = 0;

	g_rtp_packet_queue.wait_dequeue(message);

	assert(message.size() > 0);
	assert((int)message.size() <= buf_size);

	count_rtp_packets += 1;
	count_rtp_bytes += message.size();
	if ((count_rtp_packets % 10000) == 0) {
		std::cout
			<< "[readAvioPacketRTP] [Thread "
			<< std::this_thread::get_id()
			<< "] Received "
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

static bool setupFfmpeg()
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

	// Discard frames marked as corrupt.
	rtp_fctx->flags |= AVFMT_FLAG_DISCARD_CORRUPT;

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

	return true;
}

static void runPlplay()
{
	int ret = plplay_play(rtp_fctx);
	std::cerr
		<< "[runPlplay] plplay_play() exited with return code "
		<< ret
		<< std::endl;
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
			std::jthread thr_plplay_worker(runPlplay);
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

	for (;;) {
		char tmp;
		std::cin >> tmp;
	}

	return 0;
}
