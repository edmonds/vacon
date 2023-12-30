#pragma once

#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include "common.hpp"
#include "vpacket.hpp"

namespace vacon {

struct NetworkHandlerParams {
    std::string signaling_base_url;
    std::string signaling_secret;
    std::string stun_server;
};

class NetworkHandler {
    public:
        static std::unique_ptr<NetworkHandler> Create(const NetworkHandlerParams& params);
        NetworkHandler(NetworkHandler&&) = default;
        ~NetworkHandler();

        void connectWebRTC();
        bool isConnectedToPeer();
        void closeWebSocket();
        void sendPacket(std::shared_ptr<VPacket>);

    private:
        NetworkHandler() = default;

        NetworkHandlerParams params;

        rtc::Configuration                              config;
        std::shared_ptr<rtc::WebSocket>                 ws;
        std::shared_ptr<rtc::PeerConnection>            peer;
        std::shared_ptr<rtc::RtcpSrReporter>            sender_reporter;
        std::shared_ptr<rtc::RtpPacketizationConfig>    rtp_config;
        std::shared_ptr<rtc::Track>                     track;

        void onWsMessage(nlohmann::json message);
        void createPeerConnection(const std::optional<rtc::Description>& offer = std::nullopt);
};

} // namespace vacon
