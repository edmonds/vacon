// Copyright (c) 2023-2024 The Vacon Authors
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

#include "network_handler.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include "common.hpp"

using namespace std::chrono_literals;

using nlohmann::json;

namespace vacon {

std::unique_ptr<NetworkHandler> NetworkHandler::Create(const NetworkHandlerParams& params)
{
    PLOG_DEBUG <<
        fmt::format("NetworkHandlerParams: signaling_base_url {}, signaling secret {}, stun_server {}",
                    params.signaling_base_url,
                    params.signaling_secret,
                    params.stun_server);

    auto nh = std::make_unique<NetworkHandler>(NetworkHandler {});
    nh->params = params;
    nh->config.iceServers.emplace_back(nh->params.stun_server);
    return nh;
}

NetworkHandler::~NetworkHandler()
{
    PLOG_VERBOSE << fmt::format("Destructor called on {}", fmt::ptr(this));
}

void NetworkHandler::connectWebRTC()
{
    ws = std::make_shared<rtc::WebSocket>();

    ws->onOpen([]() {
        PLOG_INFO << "WebSocket connected, signaling ready";
    });

    ws->onError([](std::string s) {
        PLOG_INFO << "WebSocket error: " << s;
    });

    ws->onMessage([&](std::variant<rtc::binary, rtc::string> data) {
        if (std::holds_alternative<rtc::string>(data)) {
            auto string_data = std::get<rtc::string>(data);
            json message = json::parse(std::get<rtc::string>(data));
            this->onWsMessage(message);
        }
    });

    const std::string url = params.signaling_base_url + "/" + params.signaling_secret;
    PLOG_INFO << fmt::format("Opening WebSocket URL {}", url);
    ws->open(url);
}

bool NetworkHandler::isConnectedToPeer()
{
    if (peer) {
        return peer->state() == rtc::PeerConnection::State::Connected;
    }
    return false;
}

void NetworkHandler::closeWebSocket()
{
    if (ws->isOpen()) {
        ws->close();
    }
}

void NetworkHandler::onWsMessage(json message)
{
    PLOG_DEBUG << "Received WebSocket message: " << message.dump();

    if (!message.contains("type")) {
        PLOG_ERROR << "Message lacks key 'type'";
        return;
    }
    auto type = message.find("type")->get<std::string>();

    if (type == "start_session") {
        PLOG_DEBUG << "Got start_session, creating peer connection and sending offer";
        createPeerConnection();
        return;
    }

    if (type == "offer") {
        PLOG_DEBUG << "Got offer, creating peer connection and sending answer";
        auto sdp = message["sdp"].get<std::string>();
        auto description = rtc::Description(sdp, type);
        createPeerConnection(std::optional<rtc::Description>(description));
    } else if (type == "answer") {
        PLOG_DEBUG << "Got answer, completing session startup";
        auto sdp = message["sdp"].get<std::string>();
        auto description = rtc::Description(sdp, type);
        peer->setRemoteDescription(description);
    }
}

void NetworkHandler::createPeerConnection(const std::optional<rtc::Description>& offer)
{
    peer = std::make_shared<rtc::PeerConnection>(config);

    rtp_depacketizer = vacon::RtpDepacketizer::Create();

    auto wws = make_weak_ptr(ws);

    peer->onGatheringStateChange([&](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            auto description = peer->localDescription();
            json message = {
                { "type", description->typeString() },
                { "sdp", std::string(description.value()) },
            };
            auto message_dump = message.dump();
            PLOG_DEBUG << "[PeerConnection] Sending WebSocket message: " << message_dump;
            if (auto ws = wws.lock()) {
                ws->send(message_dump);
            }
        }
    });

    if (offer) {
        peer->onLocalDescription([](rtc::Description description) {
            json message = {
                { "type", description.typeString() },
                { "sdp", std::string(description) },
            };
            PLOG_DEBUG << "[PeerConnection onLocalDescription] Local Description: " << message.dump();
        });
    }

    const rtc::SSRC ssrc = 42;
    const int video_payload_type = 96;

    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
    video.addH265Codec(video_payload_type);
    video.addSSRC(ssrc, "video");
    track = peer->addTrack(video);

    rtp_config = std::make_shared<rtc::RtpPacketizationConfig>
        (ssrc, "video", video_payload_type, rtc::H265RtpPacketizer::defaultClockRate);

    auto packetizer = std::make_shared<rtc::H265RtpPacketizer>
        (rtc::H265RtpPacketizer::Separator::LongStartSequence, rtp_config);

    sender_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
    packetizer->addToChain(sender_reporter);

    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);

    auto session = std::make_shared<rtc::RtcpReceivingSession>();
    packetizer->addToChain(session);

    track->setMediaHandler(packetizer);

    track->onMessage([&](rtc::binary pkt) { this->receivePacket(pkt); }, nullptr);

    if (offer) {
        peer->setRemoteDescription(offer.value());
    } else {
        peer->setLocalDescription();
    }
}

void NetworkHandler::receivePacket(rtc::binary pkt)
{
    // This is an RTP packet.
    PLOG_VERBOSE << fmt::format("RECEIVED AN RTP PACKET !!! length {}", pkt.size());
    if (this->rtp_depacketizer) {
        this->rtp_depacketizer->submitRtpPacket(pkt);
    }
}

void NetworkHandler::sendPacket(std::shared_ptr<VPacket> pkt)
{
    // Only send the packet if the connection is open.
    if (!track || !track->isOpen()) {
        return;
    }

    // Sample time is in microseconds, convert it to seconds.
    auto elapsedSeconds = double(pkt->ptr->pts) / (1'000'000);

    // Get elapsed time in clock rate.
    uint32_t elapsedTimestamp = rtp_config->secondsToTimestamp(elapsedSeconds);

    // Set new timestamp.
    rtp_config->timestamp = rtp_config->startTimestamp + elapsedTimestamp;

    // Get elapsed time in clock rate from last RTCP sender report.
    auto reportElapsedTimestamp = rtp_config->timestamp - sender_reporter->lastReportedTimestamp();

    // Check if last report was at least 1 second ago.
    if (rtp_config->timestampToSeconds(reportElapsedTimestamp) > 1) {
        sender_reporter->setNeedsToReport();
    }

    // Send the packet.
    try {
        auto data = reinterpret_cast<const std::byte *>(pkt->ptr->data);
        auto size = pkt->ptr->size;
        PLOG_VERBOSE << fmt::format("Sending packet @ {}, size {}", fmt::ptr(data), size);
        track->send(data, size);
    } catch (const std::exception &e) {
        PLOG_INFO << "Unable to send packet: " << e.what();
    }
}

AVFormatContext* NetworkHandler::getRtpAvfcInput()
{
    if (rtp_depacketizer) {
        return rtp_depacketizer->fctx;
    } else {
        return nullptr;
    }
}

} // namespace vacon
