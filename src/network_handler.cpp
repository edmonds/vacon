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
#include <thread>
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
    nh->params_ = params;
    nh->config_.iceServers.emplace_back(nh->params_.stun_server);

    nh->outgoing_video_packet_queue_ =
        std::make_shared<moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<linux::VideoFrame>>>(2);

    return nh;
}

NetworkHandler::~NetworkHandler()
{
    PLOG_VERBOSE << fmt::format("Destructor called on {}", fmt::ptr(this));
}

void NetworkHandler::Init()
{
    threads_.emplace_back(std::jthread { [&](std::stop_token st) { RunDrain(st); } });
}

void NetworkHandler::Stop()
{
    for (auto& thread : threads_) {
        thread.request_stop();
    }
}

void NetworkHandler::Join()
{
    PLOG_INFO << "Waiting for network handler threads to exit...";

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            PLOG_DEBUG << "Trying to join thread ID " << thread.get_id();
            thread.join();
        } else {
            PLOG_FATAL << "Thread ID " << thread.get_id() << " is not joinable ?!";
        }
    }
}

void NetworkHandler::RunDrain(std::stop_token st)
{
    PLOG_DEBUG << "Starting outgoing video packet queue drain thread ID " << std::this_thread::get_id();
    setThreadName("VOutVideo");

    auto t_last = std::chrono::steady_clock::now();
    int count_frames = -1;

    while (!st.stop_requested()) {
        std::shared_ptr<linux::VideoFrame> frame;
        if (outgoing_video_packet_queue_->wait_dequeue_timed(frame, 250ms)) {
            SendVideoFrame(frame->CompressedData(), frame->CompressedDataLength(), frame->pts);

            auto t_now = std::chrono::steady_clock::now();
            if (count_frames == -1) {
                t_last = t_now;
            }
            ++count_frames;
            auto t_dur = t_now - t_last;

            if (t_dur >= 1s) {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_dur).count();
                auto fps = count_frames / std::chrono::duration<double>(t_dur).count();
                PLOG_INFO << fmt::format("Processed {} outgoing camera frames in {} ms, {:.3f} fps",
                                         count_frames, ms, fps);
                t_last = t_now;
                count_frames = 0;
            }
        } else {
            PLOG_VERBOSE << "Stalled dequeuing packet from outgoing video packet queue, retrying";
        }
    }

    PLOG_DEBUG << "Stopping outgoing video packet queue drain thread ID " << std::this_thread::get_id();
}

void NetworkHandler::ConnectWebRTC()
{
    ws_ = std::make_shared<rtc::WebSocket>();

    ws_->onOpen([]() {
        PLOG_INFO << "WebSocket connected, signaling ready";
    });

    ws_->onError([](std::string s) {
        PLOG_INFO << "WebSocket error: " << s;
    });

    ws_->onMessage([&](std::variant<rtc::binary, rtc::string> data) {
        if (std::holds_alternative<rtc::string>(data)) {
            auto string_data = std::get<rtc::string>(data);
            json message = json::parse(std::get<rtc::string>(data));
            OnWsMessage(message);
        }
    });

    const std::string url = params_.signaling_base_url + "/" + params_.signaling_secret;
    PLOG_INFO << fmt::format("Opening WebSocket URL {}", url);
    ws_->open(url);
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
    PLOG_DEBUG << "Received WebSocket message: " << message.dump();

    if (!message.contains("type")) {
        PLOG_ERROR << "Message lacks key 'type'";
        return;
    }
    auto type = message.find("type")->get<std::string>();

    if (type == "start_session") {
        PLOG_DEBUG << "Got start_session, creating peer connection and sending offer";
        CreatePeerConnection();
        return;
    }

    if (type == "offer") {
        PLOG_DEBUG << "Got offer, creating peer connection and sending answer";
        auto sdp = message["sdp"].get<std::string>();
        auto description = rtc::Description(sdp, type);
        CreatePeerConnection(std::optional<rtc::Description>(description));
    } else if (type == "answer") {
        PLOG_DEBUG << "Got answer, completing session startup";
        auto sdp = message["sdp"].get<std::string>();
        auto description = rtc::Description(sdp, type);
        peer_->setRemoteDescription(description);
    }
}

void NetworkHandler::CreatePeerConnection(const std::optional<rtc::Description>& offer)
{
    peer_ = std::make_shared<rtc::PeerConnection>(config_);

    rtp_depacketizer_ = vacon::RtpDepacketizer::Create();

    peer_->onGatheringStateChange([&, wws = make_weak_ptr(ws_)](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            auto description = peer_->localDescription();
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
        peer_->onLocalDescription([](rtc::Description description) {
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
    track_ = peer_->addTrack(video);

    rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>
        (ssrc, "video", video_payload_type, rtc::H265RtpPacketizer::defaultClockRate);

    auto packetizer = std::make_shared<rtc::H265RtpPacketizer>
        (rtc::H265RtpPacketizer::Separator::LongStartSequence, rtp_config_);

    sender_reporter_ = std::make_shared<rtc::RtcpSrReporter>(rtp_config_);
    packetizer->addToChain(sender_reporter_);

    auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
    packetizer->addToChain(nackResponder);

    auto session = std::make_shared<rtc::RtcpReceivingSession>();
    packetizer->addToChain(session);

    track_->setMediaHandler(packetizer);

    track_->onMessage([&](rtc::binary pkt) { ReceivePacket(pkt); }, nullptr);

    if (offer) {
        peer_->setRemoteDescription(offer.value());
    } else {
        peer_->setLocalDescription();
    }
}

void NetworkHandler::ReceivePacket(rtc::binary pkt)
{
    // This is an RTP packet.
    PLOG_VERBOSE << fmt::format("RECEIVED AN RTP PACKET !!! length {}", pkt.size());
    if (rtp_depacketizer_) {
        rtp_depacketizer_->submitRtpPacket(pkt);
    }
}

void NetworkHandler::SendVideoFrame(const std::byte *data, size_t size, uint64_t pts)
{
    // Only send the packet if the connection is open.
    if (!track_ || !track_->isOpen()) {
        return;
    }

    // Consistency check.
    if (!data || size == 0) {
        PLOG_DEBUG << fmt::format("Called with no data or zero length data at PTS {}, ignoring", pts);
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
        PLOG_VERBOSE << fmt::format("Sending packet @ {}, size {}", fmt::ptr(data), size);
        track_->send(data, size);
    } catch (const std::exception &e) {
        PLOG_INFO << "Unable to send packet: " << e.what();
    }
}

AVFormatContext* NetworkHandler::GetRtpAvfcInput()
{
    if (rtp_depacketizer_) {
        return rtp_depacketizer_->fctx;
    } else {
        return nullptr;
    }
}

} // namespace vacon
