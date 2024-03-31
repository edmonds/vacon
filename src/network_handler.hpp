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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>
#include <rtc/rtc.hpp>

#include "codecs.hpp"
#include "invite.hpp"
#include "linux/typedefs.hpp"
#include "stats.hpp"

namespace vacon {

typedef std::vector<std::pair<VideoCodec, rtc::Description::Direction>>
    CodecDirections;

struct NetworkHandlerParams {
    std::shared_ptr<Invite> invite;
    std::string stun_server;
    std::shared_ptr<linux::VideoPacketQueue> outgoing_video_packet_queue;
    std::shared_ptr<RtcPacketQueue> incoming_video_packet_queue;
    std::shared_ptr<std::vector<VideoCodec>> decoder_codecs;
    std::shared_ptr<std::vector<VideoCodec>> encoder_codecs;
};

class NetworkHandler {
    public:
        static std::unique_ptr<NetworkHandler> Create(const NetworkHandlerParams& params);
        NetworkHandler(NetworkHandler&&) = default;
        ~NetworkHandler();
        void StartDrainThread();
        void StartConnectThread();

        VideoCodec WantedDecoder()
        {
            return wanted_decoder_;
        };

        VideoCodec WantedEncoder()
        {
            return wanted_encoder_;
        };

        Welford                                         s_recv_fps_ = {};
        Welford                                         s_send_fps_ = {};

    private:
        NetworkHandler() = default;
        void ConnectWebRTC();
        void CloseWebSocket();
        bool IsConnectedToPeer();
        void RunConnect(std::stop_token);
        void RunOutgoingDrain(std::stop_token);
        void OnWsMessage(nlohmann::json message);
        void CreatePeerConnection(std::optional<rtc::Description> offer = std::nullopt);
        void ReceiveVideoPacket(rtc::binary msg, rtc::FrameInfo frame_info);
        void SendVideoPacket(const std::byte *data, size_t size, uint64_t pts);
        CodecDirections GetCodecDirections();
        std::pair<VideoCodec, int> BestDecoderFromDescription(rtc::Description&);
        std::pair<VideoCodec, int> BestEncoderFromDescription(rtc::Description&);
        void SetupVideoTracks(rtc::Description&);

        NetworkHandlerParams                            params_ = {};
        CodecDirections                                 codec_directions_ = {};
        bool                                            starting_ = false;
        std::vector<std::jthread>                       threads_ = {};
        rtc::Configuration                              config_ = {};
        std::shared_ptr<rtc::WebSocket>                 ws_ = nullptr;
        std::shared_ptr<rtc::PeerConnection>            peer_ = nullptr;
        std::shared_ptr<rtc::RtcpSrReporter>            sender_reporter_ = nullptr;
        std::shared_ptr<rtc::RtpPacketizationConfig>    rtp_config_ = nullptr;
        std::shared_ptr<rtc::Track>                     track_recv_ = nullptr;
        std::shared_ptr<rtc::Track>                     track_send_ = nullptr;
        std::vector<std::shared_ptr<rtc::Track>>        tracks_ = {};
        VideoCodec                                      wanted_decoder_ = VideoCodec::UNKNOWN;
        VideoCodec                                      wanted_encoder_ = VideoCodec::UNKNOWN;

        struct {
            ssize_t                                     n_frames_recv = -1;
            ssize_t                                     n_frames_send = -1;

            std::chrono::time_point<std::chrono::steady_clock>
                                                        t_last_recv = {};
            std::chrono::time_point<std::chrono::steady_clock>
                                                        t_last_send = {};
        } stats_;
};

} // namespace vacon
