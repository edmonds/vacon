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
#include "codecs.hpp"
#include "event.hpp"
#include "rtc_packet.hpp"
#include "rtc_utils.hpp"
#include "rtp/generic_depacketizer.hpp"
#include "rtp/generic_packetizer.hpp"
#include "util.hpp"

using namespace std::chrono_literals;

using nlohmann::json;

namespace vacon {

static const rtc::SSRC kFixedSsrc = 42;

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

    if (!params.decoder_codecs || params.decoder_codecs->empty()) {
        LOG_ERROR << "NetworkHandlerParams.decoder_codecs must be set and support at least one codec";
        return nullptr;
    }

    if (!params.encoder_codecs || params.encoder_codecs->empty()) {
        LOG_ERROR << "NetworkHandlerParams.encoder_codecs must be set and support at least one codec";
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
    track_recv_ = nullptr;
    track_send_ = nullptr;
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
        auto desc = rtc::Description(sdp, type);
        LogDescriptionVideo(desc, "[OnWsMessage, incoming offer]");
        CreatePeerConnection(desc);
    } else if (type == "answer") {
        LOG_DEBUG << "Got answer, completing session startup";
        auto sdp = message["sdp"].get<std::string>();
        auto desc = rtc::Description(sdp, type);
        LogDescriptionVideo(desc, "[OnWsMessage, incoming answer]");
        peer_->setRemoteDescription(desc);
        FinishSetupVideoTracksFromAnswer(desc);
    } else {
        LOG_DEBUG << std::format("Unknown message type '{}'", type);
    }
}

void NetworkHandler::CreatePeerConnection(std::optional<rtc::Description> offer)
{
    peer_ = std::make_shared<rtc::PeerConnection>(config_);

    peer_->onGatheringStateChange([&, wws = util::make_weak_ptr(ws_)](rtc::PeerConnection::GatheringState state) {
        if (state == rtc::PeerConnection::GatheringState::Complete) {
            auto description = peer_->localDescription();
            auto type = description->typeString();
            LogDescriptionVideo(*description, std::format("[onGatheringStateChange, outgoing {}]", type));

            json message = {
                { "type", type },
                { "sdp", std::string(description.value()) },
            };

            auto message_crypted = params_.invite->EncryptJson(message);
            if (message_crypted == std::vector<std::byte>{}) {
                LOG_ERROR << "Failed to encrypt binary WebSocket data";
            } else {
                LOG_DEBUG << "[onGatheringStateChange] Sending WebSocket message: " << message.dump();
                if (auto ws = wws.lock()) {
                    ws->send(message_crypted);
                }
            }
        }
    });

    if (offer) {
        peer_->onLocalDescription([](rtc::Description desc) {
            auto type = desc.typeString();
            json message = {
                { "type", type },
                { "sdp", std::string(desc) },
            };
            LogDescriptionVideo(desc, std::format("[onLocalDescription, {}]", type));
            LOG_DEBUG << "[onLocalDescription] Local Description: " << message.dump();
        });

        // The answer is being created in response to the remote peer's offer.
        // Based on the intersection of the local and remote encoder/decoder
        // capabilities, the best codec will be selected for each direction in
        // the answer.

        auto answer = SetupVideoTracksFromOffer(offer.value());
        LogDescriptionVideo(answer, "[CreatePeerConnection, answer]");
        peer_->setRemoteDescription(answer);
    } else {
        // The offer is being created. Every codec supported by the local
        // encoder/decoder needs to be added to the offer, and the remote peer
        // will select the actual codecs to be used for each direction based on
        // its local encoder/decoder capabilities.

        int video_payload_type = 101;
        // Add the OfferVideo track. This is the local peer's outgoing video.
        {
            rtc::Description::Video video("OfferVideo", rtc::Description::Direction::SendOnly);
            for (auto codec : *params_.encoder_codecs) {
                auto codec_name = ToString(codec);
                video.addVideoCodec(video_payload_type++, codec_name);
                video.addSSRC(kFixedSsrc, codec_name);
            }
            track_send_ = peer_->addTrack(video);
        }
        // Add the AnswerVideo track. This is the remote peer's incoming video.
        {
            rtc::Description::Video video("AnswerVideo", rtc::Description::Direction::RecvOnly);
            for (auto codec : *params_.decoder_codecs) {
                auto codec_name = ToString(codec);
                video.addVideoCodec(video_payload_type++, codec_name);
                video.addSSRC(kFixedSsrc + 1, codec_name);
            }
            track_recv_ = peer_->addTrack(video);
        }
        peer_->setLocalDescription();
    }
}

rtc::Description NetworkHandler::SetupVideoTracksFromOffer(rtc::Description& offer)
{
    // The answer is being created in response to the remote peer's offer.
    // Based on the intersection of the local and remote encoder/decoder
    // capabilities, the best codec will be selected for each direction in
    // the answer.

    auto answer = offer;

    // Add the OfferVideo track. This is the remote peer's incoming video.
    if (auto offer_video = DescriptionMediaByMid(answer, "OfferVideo")) {
        if (auto best = SelectBestVideoCodec(offer_video.value(), params_.decoder_codecs)) {
            wanted_decoder_ = best.value();
            auto codec_name = ToString(wanted_decoder_);
            LOG_INFO << "Wanted decoder is " << codec_name;

            track_recv_ = peer_->addTrack(offer_video.value()->reciprocate());
            track_recv_->chainMediaHandler(std::make_shared<GenericRtpDepacketizer>());
            track_recv_->onFrame([&](rtc::binary msg, rtc::FrameInfo frame_info) {
                ReceiveVideoPacket(msg, frame_info);
            });
        } else {
            LOG_WARNING << "Couldn't negotiate a compatible codec for OfferVideo";
        }
    } else {
        LOG_WARNING << "Didn't get OfferVideo :-(";
    }

    // Add the AnswerVideo track. This is the local peer's outgoing video.
    if (auto answer_video = DescriptionMediaByMid(answer, "AnswerVideo")) {
        if (auto best = SelectBestVideoCodec(answer_video.value(), params_.encoder_codecs)) {
            wanted_encoder_ = best.value();
            auto codec_name = ToString(wanted_encoder_);
            LOG_INFO << "Wanted encoder is " << codec_name;

            auto video = (*answer_video.value()).reciprocate();
            track_send_ = peer_->addTrack(video);
            rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>
                (kFixedSsrc + 1,
                 codec_name,
                 DescriptionMediaPayloadTypeByFormat(&video, codec_name),
                 GenericRtpPacketizer::defaultClockRate);
            track_send_->chainMediaHandler(std::make_shared<GenericRtpPacketizer>(rtp_config_));
        } else {
            LOG_WARNING << "Couldn't negotiate a compatible codec for AnswerVideo";
        }
    } else {
        LOG_WARNING << "Didn't get AnswerVideo :-(";
    }

    return answer;
}

void NetworkHandler::FinishSetupVideoTracksFromAnswer(rtc::Description& answer)
{
    if (!track_send_) {
        LOG_ERROR << "track_send_ not setup";
        return;
    }
    if (!track_recv_) {
        LOG_ERROR << "track_recv_ not setup";
        return;
    }

    // Set up the OfferVideo track. This is the local peer's outgoing video.
    auto offer_video = DescriptionMediaByMid(answer, "OfferVideo");
    if (!offer_video) {
        LOG_ERROR << "No media description for mid OfferVideo in answer";
        return;
    }
    wanted_encoder_ = DescriptionVideoCodec(offer_video.value());
    auto encoder_name = ToString(wanted_encoder_);
    LOG_INFO << "Wanted encoder is " << encoder_name;
    auto payload_type = DescriptionMediaPayloadTypeByFormat(offer_video.value(), encoder_name);
    if (payload_type == -1) {
        LOG_ERROR << "Could not find payload type for codec {} in OfferVideo";
        return;
    }
    rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>
        (kFixedSsrc, encoder_name, payload_type, GenericRtpPacketizer::defaultClockRate);
    track_send_->setMediaHandler(std::make_shared<GenericRtpPacketizer>(rtp_config_));

    // Set up the AnswerVideo track. This is the remote peer's incoming video.
    auto answer_video = DescriptionMediaByMid(answer, "AnswerVideo");
    if (!answer_video) {
        LOG_ERROR << "No media description for mid AnswerVideo in answer";
        return;
    }
    wanted_decoder_ = DescriptionVideoCodec(answer_video.value());;
    auto decoder_name = ToString(wanted_decoder_);
    LOG_INFO << "Wanted decoder is " << decoder_name;
    track_recv_->chainMediaHandler(std::make_shared<GenericRtpDepacketizer>());
    track_recv_->onFrame([&](rtc::binary msg, rtc::FrameInfo frame_info) {
        ReceiveVideoPacket(msg, frame_info);
    });
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
    if (!track_send_ || !track_send_->isOpen()) {
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
    // auto reportElapsedTimestamp = rtp_config_->timestamp - sender_reporter_->lastReportedTimestamp();

    // Check if last report was at least 1 second ago.
    // if (rtp_config_->timestampToSeconds(reportElapsedTimestamp) > 1) {
        // sender_reporter_->setNeedsToReport();
    // }

    // Send the packet.
    try {
        LOG_VERBOSE << std::format("Sending packet @ {}, size {}", (void*)data, size);
        track_send_->send(data, size);
    } catch (const std::exception &e) {
        LOG_INFO << "Unable to send packet: " << e.what();
    }
}

} // namespace vacon
