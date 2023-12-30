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

bool NetworkHandler::isConnected()
{
    if (peer) {
        return peer->state() == rtc::PeerConnection::State::Connected;
    }
    return false;
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

        // The answer has been received. Close down the WebSocket connection to
        // the signaling server.
        ws->close();
    }
}

void NetworkHandler::createPeerConnection(const std::optional<rtc::Description>& offer)
{
    peer = std::make_shared<rtc::PeerConnection>(config);

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
                if (offer) {
                    // The answer has been sent. Close down the WebSocket
                    // connection to the signaling server.
                    ws->close();
                }
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

    track->onMessage([](rtc::binary message) {
        // This is an RTP packet.
        PLOG_INFO << fmt::format("RECEIVED AN RTP PACKET !!! length {}", message.size());
    }, nullptr);

    if (offer) {
        peer->setRemoteDescription(offer.value());
    } else {
        peer->setLocalDescription();
    }
}

} // namespace vacon
