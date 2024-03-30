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
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>

#include <nlohmann/json.hpp>
#include <plog/Log.h>
#include <rtc/rtc.hpp>

#include "app.hpp"
#include "event.hpp"
#include "rtc_packet.hpp"
#include "rtp/generic_depacketizer.hpp"
#include "rtp/generic_packetizer.hpp"
#include "util.hpp"

using namespace std::chrono_literals;

using nlohmann::json;

namespace vacon {

std::unique_ptr<NetworkHandler> NetworkHandler::Create(const NetworkHandlerParams& params)
{
    if (!params.invite) {
        LOG_ERROR << "NetworkHandlerParams.invite must be set";
        return nullptr;
    }

    if (!params.incoming_video_packet_queue) {
        LOG_ERROR << "NetworkHandlerParams.incoming_video_packet_queue must be set";
        return nullptr;
    }

    if (!params.outgoing_video_packet_queue) {
        LOG_ERROR << "NetworkHandlerParams.outgoing_video_packet_queue must be set";
        return nullptr;
    }

    auto nh = std::make_unique<NetworkHandler>(NetworkHandler {});
    nh->params_ = params;
    nh->config_.iceServers.emplace_back(nh->params_.stun_server);

    return nh;
}

NetworkHandler::~NetworkHandler()
{
    if (threads_.size() == 0) {
        return;
    }

    LOG_INFO << "Waiting for network handler threads to exit...";

    for (auto& thread : threads_) {
        thread.request_stop();
    }

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            LOG_DEBUG << "Trying to join thread ID " << thread.get_id();
            thread.join();
        } else {
            LOG_FATAL << "Thread ID " << thread.get_id() << " is not joinable ?!";
        }
    }

    threads_.clear();
    peer_ = nullptr;
    track_ = nullptr;
}

void NetworkHandler::StartDrainThread()
{
    threads_.emplace_back(std::jthread { [&](std::stop_token st) { RunOutgoingDrain(st); } });
}

void NetworkHandler::StartConnectThread()
{
    if (starting_) {
        return;
    }
    starting_ = true;

    threads_.emplace_back(std::jthread { [&](std::stop_token st) { RunConnect(st); } });
}

void NetworkHandler::RunConnect(std::stop_token st)
{
    LOG_DEBUG << "Starting WebRTC connection thread ID " << std::this_thread::get_id();
    util::SetThreadName("VWebRtcConnect");

    PushEvent(Event::NetworkStarting);

    // Start connecting to the signaling server and the WebRTC peer.
    ConnectWebRTC();

    // Wait for the NetworkHandler to bring up the peer-to-peer connection.
    while (!st.stop_requested() && !IsConnectedToPeer() && !vacon::gShuttingDown) {
        std::this_thread::sleep_for(100ms);
    }

    if (IsConnectedToPeer() && !vacon::gShuttingDown) {
        LOG_FATAL << "PEER-TO-PEER CONNECTION IS READY !!!";
        PushEvent(Event::NetworkStarted);
    }

    // WebRTC peer connection is up, or we are shutting down, so close the
    // connection to the signaling server.
    CloseWebSocket();

    LOG_DEBUG << "Stopping WebRTC connection thread ID " << std::this_thread::get_id();
}

void NetworkHandler::RunOutgoingDrain(std::stop_token st)
{
    LOG_DEBUG << "Starting outgoing video packet queue drain thread ID " << std::this_thread::get_id();
    util::SetThreadName("VOutVideo");

    while (!st.stop_requested()) {
        auto t_now = std::chrono::steady_clock::now();

        std::shared_ptr<linux::VideoFrame> frame;
        if (params_.outgoing_video_packet_queue->wait_dequeue_timed(frame, 250ms)) {
            SendVideoPacket(frame->CompressedData(), frame->CompressedDataLength(), frame->pts);

            // Stats.
            if (stats_.n_frames_send++ == -1) [[unlikely]] {
                stats_.t_last_send = t_now;
            }
            auto t_dur = t_now - stats_.t_last_send;
            if (t_dur >= 1s) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_dur).count();
                auto fps = stats_.n_frames_send / std::chrono::duration<double>(t_dur).count();
                s_send_fps_.Update(fps);
                LOG_VERBOSE << std::format("Processed {} outgoing video packets in {} ms, {:.3f} fps",
                                           stats_.n_frames_send, ms, fps);
                stats_.t_last_send = t_now;
                stats_.n_frames_send = 0;
            }
        } else {
            LOG_VERBOSE << "Stalled dequeuing packet from outgoing video packet queue, retrying";
        }
    }

    LOG_DEBUG << "Stopping outgoing video packet queue drain thread ID " << std::this_thread::get_id();
}

void NetworkHandler::ConnectWebRTC()
{
    ws_ = std::make_shared<rtc::WebSocket>();

    ws_->onOpen([]() {
        LOG_INFO << "WebSocket connected, signaling ready";
    });

    ws_->onError([](std::string s) {
        LOG_INFO << "WebSocket error: " << s;
    });

    ws_->onMessage([&](std::variant<rtc::binary, rtc::string> data) {
        if (std::holds_alternative<rtc::binary>(data)) {
            const auto &binary_data = std::get<rtc::binary>(data);
            if (binary_data.size() == 1 && binary_data[0] == std::byte{0}) {
                LOG_DEBUG << "Got session start indicator, creating peer connection and sending offer";
                CreatePeerConnection();
            } else {
                json message = params_.invite->DecryptJson(binary_data);
                if (message == json{}) {
                    LOG_ERROR << "Failed to decrypt binary WebSocket data with invite key";
                } else {
                    OnWsMessage(message);
                }
            }
        } else if (std::holds_alternative<rtc::string>(data)) {
            auto string_data = std::get<rtc::string>(data);
            LOG_DEBUG << "Expecting binary WebSocket data but received string data instead: " << string_data;
        }
    });

    auto session_url = params_.invite->SessionUrl();
    LOG_INFO << "Opening WebSocket URL " << session_url;
    ws_->open(session_url);
}

bool NetworkHandler::IsConnectedToPeer()
{
    if (peer_) {
        return peer_->state() == rtc::PeerConnection::State::Connected;
    }
    return false;
}

void NetworkHandler::CloseWebSocket()
{
    if (ws_->isOpen()) {
        ws_->close();
    }
}

void NetworkHandler::OnWsMessage(json message)
{
    LOG_DEBUG << "Received WebSocket message: " << message.dump();

    if (!message.contains("type")) {
        LOG_ERROR << "Got JSON message, but key 'type' missing";
        return;
    }
    auto type = message.find("type")->get<std::string>();

    if (type == "offer") {
        LOG_DEBUG << "Got offer, creating peer connection and sending answer";
        auto sdp = message["sdp"].get<std::string>();
        auto description = rtc::Description(sdp, type);
        CreatePeerConnection(std::optional<rtc::Description>(description));
    } else if (type == "answer") {
        LOG_DEBUG << "Got answer, completing session startup";
        auto sdp = message["sdp"].get<std::string>();
        auto description = rtc::Description(sdp, type);
        peer_->setRemoteDescription(description);
    } else {
        LOG_DEBUG << std::format("Unknown message type '{}'", type);
    }
}

void NetworkHandler::CreatePeerConnection(const std::optional<rtc::Description>& offer)
{
    peer_ = std::make_shared<rtc::PeerConnection>(config_);

    peer_->onGatheringStateChange([&, wws = util::make_weak_ptr(ws_)](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            auto description = peer_->localDescription();
            json message = {
                { "type", description->typeString() },
                { "sdp", std::string(description.value()) },
            };
            auto message_crypted = params_.invite->EncryptJson(message);
            if (message_crypted == std::vector<std::byte>{}) {
                LOG_ERROR << "Failed to encrypt binary WebSocket data";
            } else {
                LOG_DEBUG << "[PeerConnection] Sending WebSocket message: " << message.dump();
                if (auto ws = wws.lock()) {
                    ws->send(message_crypted);
                }
            }
        }
    });

    if (offer) {
        peer_->onLocalDescription([](rtc::Description description) {
            json message = {
                { "type", description.typeString() },
                { "sdp", std::string(description) },
            };
            LOG_DEBUG << "[PeerConnection onLocalDescription] Local Description: " << message.dump();
        });
    }

    const rtc::SSRC ssrc = 42;
    const int video_payload_type = 96;

    rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>
        (ssrc, "video", video_payload_type, GenericRtpPacketizer::defaultClockRate);

    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
    video.addVideoCodec(video_payload_type, "H265");
    video.addSSRC(ssrc, "video");

    track_ = peer_->addTrack(video);

    auto packetizer = std::make_shared<GenericRtpPacketizer>(rtp_config_);
    auto depacketizer = std::make_shared<GenericRtpDepacketizer>();
    sender_reporter_ = std::make_shared<rtc::RtcpSrReporter>(rtp_config_);
    auto nack_responder = std::make_shared<rtc::RtcpNackResponder>();
    auto session = std::make_shared<rtc::RtcpReceivingSession>();

    track_->setMediaHandler(packetizer);
    track_->chainMediaHandler(depacketizer);
    track_->chainMediaHandler(sender_reporter_);
    track_->chainMediaHandler(nack_responder);
    track_->chainMediaHandler(session);

    track_->onFrame([&](rtc::binary msg, rtc::FrameInfo frame_info) {
        ReceiveVideoPacket(msg, frame_info);
    });

    track_->onMessage([](rtc::binary pkt) {
        LOG_DEBUG << "Discarding unhandled incoming message, size " << pkt.size();
    }, nullptr);

    if (offer) {
        peer_->setRemoteDescription(offer.value());
    } else {
        peer_->setLocalDescription();
    }
}

void NetworkHandler::ReceiveVideoPacket(rtc::binary msg, rtc::FrameInfo frame_info)
{
    auto t_now = std::chrono::steady_clock::now();
    auto packet = RtcPacket::Create(msg, frame_info);

    LOG_VERBOSE << std::format("Received video packet, size {}, timestamp {}",
                               msg.size(), frame_info.timestamp);

    // Enqueue the incoming video packet.
    while (!vacon::gShuttingDown) {
        if (params_.incoming_video_packet_queue->wait_enqueue_timed(packet, 250ms)) {
            break;
        } else {
            LOG_DEBUG << "Stalled enqueuing packet onto incoming video packet queue, retrying";
        }
    }

    // Stats.
    if (stats_.n_frames_recv++ == -1) [[unlikely]] {
        stats_.t_last_recv = t_now;
    } else {
        auto t_dur = t_now - stats_.t_last_recv;
        if (t_dur >= 1s) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_dur).count();
            auto fps = stats_.n_frames_recv / std::chrono::duration<double>(t_dur).count();
            s_recv_fps_.Update(fps);
            LOG_VERBOSE << std::format("Processed {} incoming video packets in {} ms, {:.3f} fps",
                                       stats_.n_frames_recv, ms, fps);
            stats_.t_last_recv = t_now;
            stats_.n_frames_recv = 0;
        }
    }
}

void NetworkHandler::SendVideoPacket(const std::byte *data, size_t size, uint64_t pts)
{
    // Only send the packet if the connection is open.
    if (!track_ || !track_->isOpen()) {
        return;
    }

    // Consistency check.
    if (!data || size == 0) {
        LOG_DEBUG << std::format("Called with no data or zero length data at PTS {}, ignoring", pts);
        return;
    }

    // Sample time is in microseconds, convert it to seconds.
    auto elapsedSeconds = double(pts) / (1'000'000);

    // Get elapsed time in clock rate.
    uint32_t elapsedTimestamp = rtp_config_->secondsToTimestamp(elapsedSeconds);

    // Set new timestamp.
    rtp_config_->timestamp = rtp_config_->startTimestamp + elapsedTimestamp;

    // Get elapsed time in clock rate from last RTCP sender report.
    auto reportElapsedTimestamp = rtp_config_->timestamp - sender_reporter_->lastReportedTimestamp();

    // Check if last report was at least 1 second ago.
    if (rtp_config_->timestampToSeconds(reportElapsedTimestamp) > 1) {
        sender_reporter_->setNeedsToReport();
    }

    // Send the packet.
    try {
        LOG_VERBOSE << std::format("Sending packet @ {}, size {}", (void*)data, size);
        track_->send(data, size);
    } catch (const std::exception &e) {
        LOG_INFO << "Unable to send packet: " << e.what();
    }
}

} // namespace vacon
