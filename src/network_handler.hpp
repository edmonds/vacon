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

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>
#include <rtc/rtc.hpp>

#include "common.hpp"
#include "rtp_depacketizer.hpp"
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

        AVFormatContext* getRtpAvfcInput();

    private:
        NetworkHandler() = default;
        void receivePacket(rtc::binary);

        NetworkHandlerParams params;

        rtc::Configuration                              config;
        std::shared_ptr<rtc::WebSocket>                 ws;
        std::shared_ptr<rtc::PeerConnection>            peer;
        std::shared_ptr<rtc::RtcpSrReporter>            sender_reporter;
        std::shared_ptr<rtc::RtpPacketizationConfig>    rtp_config;
        std::shared_ptr<rtc::Track>                     track;

        std::shared_ptr<vacon::RtpDepacketizer>         rtp_depacketizer;

        void onWsMessage(nlohmann::json message);
        void createPeerConnection(const std::optional<rtc::Description>& offer = std::nullopt);
};

} // namespace vacon
