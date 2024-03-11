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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include <rtc/rtc.hpp>

#include "invite.hpp"
#include "linux/typedefs.hpp"
#include "rtp_depacketizer.hpp"

namespace vacon {

extern std::atomic_size_t n_network_incoming_fpks;
extern std::atomic_size_t n_network_outgoing_fpks;

struct NetworkHandlerParams {
    std::shared_ptr<Invite> invite;
    std::string stun_server;
    std::shared_ptr<linux::VideoPacketQueue> outgoing_video_packet_queue;
    std::shared_ptr<PacketRefQueue> incoming_video_packet_queue;
};

class NetworkHandler {
    public:
        static std::unique_ptr<NetworkHandler> Create(const NetworkHandlerParams& params);
        NetworkHandler(NetworkHandler&&) = default;
        ~NetworkHandler();
        void Init();
        void StartAsync();

    private:
        NetworkHandler() = default;
        void ConnectWebRTC();
        void CloseWebSocket();
        bool IsConnectedToPeer();
        void RunConnect(std::stop_token);
        void RunIncomingFill(std::stop_token);
        void RunOutgoingDrain(std::stop_token);
        void ReceivePacket(rtc::binary);
        void OnWsMessage(nlohmann::json message);
        void CreatePeerConnection(const std::optional<rtc::Description>& offer = std::nullopt);
        void SendVideoFrame(const std::byte *data, size_t size, uint64_t pts);

        NetworkHandlerParams                            params_ = {};
        bool                                            starting_ = false;
        std::vector<std::jthread>                       threads_ = {};
        rtc::Configuration                              config_;
        std::shared_ptr<rtc::WebSocket>                 ws_;
        std::shared_ptr<rtc::PeerConnection>            peer_;
        std::shared_ptr<rtc::RtcpSrReporter>            sender_reporter_;
        std::shared_ptr<rtc::RtpPacketizationConfig>    rtp_config_;
        std::shared_ptr<rtc::Track>                     track_;
        std::shared_ptr<vacon::RtpDepacketizer>         rtp_depacketizer_;
};

} // namespace vacon
