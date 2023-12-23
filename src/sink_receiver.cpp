#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

extern "C" {
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
}

using namespace std::chrono_literals;

using std::make_shared;
using std::shared_ptr;
using std::string;
using std::weak_ptr;

using nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

static shared_ptr<rtc::PeerConnection> g_pc;
static shared_ptr<rtc::WebSocket> g_ws;

static long g_receiver_count_rtp_packets;
static long g_receiver_count_rtp_bytes;

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

	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	addr.sin_port = htons(5000);

	track->onMessage([track, sock, addr](rtc::binary message) {
		// This is an RTP packet.
		g_receiver_count_rtp_packets += 1;
		g_receiver_count_rtp_bytes += message.size();
		if ((g_receiver_count_rtp_packets % 1000) == 0) {
			std::cout
				<< "Received "
				<< g_receiver_count_rtp_packets
				<< " packets, "
				<< g_receiver_count_rtp_bytes
				<< " bytes from RTP peer"
				<< std::endl;
		}
		sendto(sock,
		       reinterpret_cast<const char *>(message.data()),
		       message.size(),
		       0 /* flags */,
		       reinterpret_cast<const struct sockaddr *>(&addr),
		       sizeof(addr));
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
	}
}

int sink_receiver(const string& ws_url)
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
