/**
 * WIP
 *
 * Based on:
 *
 * libdatachannel media sender example
 * Copyright (c) 2020 Staz Modrzynski
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

using namespace std::chrono_literals;

using std::make_shared;
using std::shared_ptr;
using std::string;
using std::weak_ptr;

using nlohmann::json;

extern "C" {
#include <libavcodec/avcodec.h>
int start_v4l2_vaapi_encoder(const char *device, void (*cb)(AVPacket *));
}

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

static shared_ptr<rtc::PeerConnection> g_pc;
static shared_ptr<rtc::RtcpSrReporter> g_sender_srReporter;
static shared_ptr<rtc::RtpPacketizationConfig> g_sender_rtpConfig;
static shared_ptr<rtc::Track> g_sender_track;

static void packet_callback(AVPacket *pkt)
{
	if (!g_sender_track || !g_sender_track->isOpen()) {
		return;
	}

	// Sample time is in microseconds, convert it to seconds.
	auto elapsedSeconds = double(pkt->pts) / (1000 * 1000);
	// Get elapsed time in clock rate.
	uint32_t elapsedTimestamp = g_sender_rtpConfig->secondsToTimestamp(elapsedSeconds);
	// Set new timestamp.
	g_sender_rtpConfig->timestamp = g_sender_rtpConfig->startTimestamp + elapsedTimestamp;

	// Get elapsed time in clock rate from last RTCP sender report.
	auto reportElapsedTimestamp = g_sender_rtpConfig->timestamp - g_sender_srReporter->lastReportedTimestamp();
	// Check if last report was at least 1 second ago.
	if (g_sender_rtpConfig->timestampToSeconds(reportElapsedTimestamp) > 1) {
		g_sender_srReporter->setNeedsToReport();
	}

	try {
		g_sender_track->send(reinterpret_cast<const std::byte *>(pkt->data), pkt->size);
	} catch (const std::exception &e) {
		std::cerr << "Unable to send packet: " << e.what() << std::endl;
	}
}

static void createPeerConnection(const rtc::Configuration &config,
				 weak_ptr<rtc::WebSocket> wws)
{
	g_pc = make_shared<rtc::PeerConnection>(config);

	g_pc->onStateChange([](rtc::PeerConnection::State state) {
		std::cout << "[PeerConnection onStateChange] State: " << state << std::endl;
	});

	g_pc->onGatheringStateChange([wws](rtc::PeerConnection::GatheringState state) {
		std::cout << "[PeerConnection onGatheringStateChange] State: " << state << std::endl;
		if (state == rtc::PeerConnection::GatheringState::Complete) {
			auto description = g_pc->localDescription();
			json message = {
				{ "id", "client" },
				{ "type", description->typeString() },
				{ "sdp", string(description.value()) },
			};
			auto message_dump = message.dump();
			std::cout << "[PeerConnection] Sending WebSocket message: "
				<< message_dump
				<< std::endl;
			if (auto ws = wws.lock()) {
				ws->send(message_dump);
			}
		}
	});

	const rtc::SSRC ssrc = 42;
	rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
	video.addH265Codec(96);
	video.addSSRC(ssrc, "video");
	g_sender_track = g_pc->addTrack(video);

	g_sender_rtpConfig = make_shared
		<rtc::RtpPacketizationConfig>
			(ssrc, "video", 96, rtc::H265RtpPacketizer::defaultClockRate);

	auto packetizer = make_shared
		<rtc::H265RtpPacketizer>
			(rtc::H265RtpPacketizer::Separator::LongStartSequence, g_sender_rtpConfig);

	g_sender_srReporter = make_shared<rtc::RtcpSrReporter>(g_sender_rtpConfig);
	packetizer->addToChain(g_sender_srReporter);

	auto nackResponder = make_shared<rtc::RtcpNackResponder>();
	packetizer->addToChain(nackResponder);

	g_sender_track->setMediaHandler(packetizer);

	g_pc->setLocalDescription();
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

	if (type == "request") {
		createPeerConnection(config, make_weak_ptr(ws));
	} else if (type == "answer") {
		auto sdp = message["sdp"].get<string>();
		auto description = rtc::Description(sdp, type);
		g_pc->setRemoteDescription(description);

		ws->close();
	}
}

int camera_sender(const string& ws_url)
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

	//const string url = "ws://10.1.10.89:5201/server";
	const string url = ws_url + "/server";
	std::cout << "Opening WebSocket URL " << url << std::endl;
	ws->open(url);

	std::cout << "Waiting for signaling to be connected..." << std::endl;
	wsFuture.get();

	int ret = start_v4l2_vaapi_encoder("/dev/video0", packet_callback);
	if (ret != 0) {
		std::cerr << "start_v4l2_vaapi_encoder() failed!" << std::endl;
		return EXIT_FAILURE;
	}

	return 0;
}
