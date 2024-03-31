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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>
#include <plog/Log.h>
#include <rtc/rtc.hpp>

#include "app.hpp"
#include "codecs.hpp"
#include "event.hpp"
#include "rtc_packet.hpp"
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
    nh->codec_directions_ = nh->GetCodecDirections();

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

static void LogDescriptionVideo(rtc::Description& desc)
{
    for (unsigned int i = 0; i < desc.mediaCount(); ++i) {
        if (std::holds_alternative<rtc::Description::Media*>(desc.media(i))) {
            auto media = std::get<rtc::Description::Media*>(desc.media(i));
            auto payload_types = media->payloadTypes();
            for (auto payload_type : payload_types) {
                const auto rtp_map = media->rtpMap(payload_type);
                LOG_DEBUG << std::format("Video #{}, payload type {}, format \"{}\"",
                                         i, payload_type, rtp_map->format);
            }
        }
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
        CreatePeerConnection(desc);

        LOG_DEBUG << "Setting up video tracks based on incoming offer";
        SetupVideoTracks(desc);
    } else if (type == "answer") {
        LOG_DEBUG << "Got answer, completing session startup";
        auto sdp = message["sdp"].get<std::string>();
        auto desc = rtc::Description(sdp, type);

        LOG_DEBUG << "Setting up video tracks based on incoming answer";
        SetupVideoTracks(desc);
        peer_->setRemoteDescription(desc);
    } else {
        LOG_DEBUG << std::format("Unknown message type '{}'", type);
    }
}

static std::vector<rtc::Description::Media*> GetVideosFromDescription(rtc::Description &desc)
{
    auto res = std::vector<rtc::Description::Media*>();
    for (unsigned int i = 0; i < desc.mediaCount(); ++i) {
        if (std::holds_alternative<rtc::Description::Media*>(desc.media(i))) {
            auto media = std::get<rtc::Description::Media*>(desc.media(i));
            if (media->type() != "video") {
                continue;
            }
            res.push_back(media);
        }
    }
    return res;
}

static VideoCodec GetVideoCodecFromMedia(rtc::Description::Media* media)
{
    auto payload_types = media->payloadTypes();
    if (payload_types.size() != 1) {
        return VideoCodec::UNKNOWN;
    }
    auto payload_type = payload_types.front();
    const auto rtp_map = media->rtpMap(payload_type);
    return FromString(rtp_map->format);
}

static int GetPayloadTypeFromMedia(rtc::Description::Media* media)
{
    auto payload_types = media->payloadTypes();
    if (payload_types.size() != 1) {
        return -1;
    }
    return payload_types.front();
}

void NetworkHandler::SetupVideoTracks(rtc::Description& desc)
{
    LogDescriptionVideo(desc);

    for (auto video : GetVideosFromDescription(desc)) {
        auto codec = GetVideoCodecFromMedia(video);

        if (codec == VideoCodec::UNKNOWN) {
            continue;
        }

        switch (video->direction()) {
        case rtc::Description::Direction::SendRecv:
            if (wanted_decoder_ == VideoCodec::UNKNOWN) {
                wanted_decoder_ = codec;
            }
            if (wanted_encoder_ == VideoCodec::UNKNOWN) {
                wanted_encoder_ = codec;
            }
            break;

        case rtc::Description::Direction::SendOnly:
            if (wanted_encoder_ == VideoCodec::UNKNOWN) {
                wanted_encoder_ = codec;
            }
            break;

        case rtc::Description::Direction::RecvOnly:
            if (wanted_decoder_ == VideoCodec::UNKNOWN) {
                wanted_decoder_ = codec;
            }
            break;

        default:
            break;
        }
    }

    if (wanted_decoder_ == VideoCodec::UNKNOWN) {
        LOG_ERROR << "Couldn't determine decoder to use from description";
        return;
    }

    if (wanted_encoder_ == VideoCodec::UNKNOWN) {
        LOG_ERROR << "Couldn't determine encoder to use from description";
        return;
    }

    LOG_INFO << std::format("Want to use {} for decoding", ToString(wanted_decoder_));
    LOG_INFO << std::format("Want to use {} for encoding", ToString(wanted_encoder_));

    for (auto track : tracks_) {
        auto media = track->description();
        auto media_codec = GetVideoCodecFromMedia(&media);

        // Skip if track doesn't match the wanted decoder or wanted encoder.
        if (media_codec != wanted_decoder_ &&
            media_codec != wanted_encoder_)
        {
            continue;
        }

        if (wanted_decoder_ == wanted_encoder_) {
            // Consistency check.
            if (track->direction() != rtc::Description::Direction::SendRecv) {
                LOG_ERROR << "Wanted decoder is same as wanted encoder but track is not SendRecv direction";
                break;
            }

            rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>
                (kFixedSsrc,
                 ToString(media_codec),
                 GetPayloadTypeFromMedia(&media),
                 GenericRtpPacketizer::defaultClockRate);

            // This is a SendRecv track so a packetizer is needed for the sent
            // frames.
            auto packetizer = std::make_shared<GenericRtpPacketizer>(rtp_config_);
            track->setMediaHandler(packetizer);

            // This is a SendRecv track so a de-packetizer is needed for the
            // received frames.
            auto depacketizer = std::make_shared<GenericRtpDepacketizer>();
            track->chainMediaHandler(depacketizer);

            // This is a SendRecv track so an onFrame callback is needed to
            // process the incoming video frames.
            track->onFrame([&](rtc::binary msg, rtc::FrameInfo frame_info) {
                ReceiveVideoPacket(msg, frame_info);
            });

            // Use this track for both sending and receiving.
            track_recv_ = track;
            track_send_ = track;
            // track_ = track;

            // Looking at the remaining tracks is unnecessary.
            break;
        } else if (media_codec == wanted_decoder_) {
            // Consistency check.
            if (track->direction() != rtc::Description::Direction::RecvOnly) {
                LOG_ERROR << "Track for wanted decoder is not RecvOnly direction";
                break;
            }

            // This is a RecvOnly track so only a de-packetizer is needed for
            // the received frames.
            auto depacketizer = std::make_shared<GenericRtpDepacketizer>();
            track->chainMediaHandler(depacketizer);

            // This is a RecvOnly track so an onFrame callback is needed to
            // process the incoming video frames.
            track->onFrame([&](rtc::binary msg, rtc::FrameInfo frame_info) {
                ReceiveVideoPacket(msg, frame_info);
            });

            // Use this track for receiving.
            track_recv_ = track;
        } else if (media_codec == wanted_encoder_) {
            // Consistency check.
            if (track->direction() != rtc::Description::Direction::SendOnly) {
                LOG_ERROR << "Track for wanted encoder is not SendOnly direction";
                break;
            }

            rtp_config_ = std::make_shared<rtc::RtpPacketizationConfig>
                (kFixedSsrc,
                 ToString(media_codec),
                 GetPayloadTypeFromMedia(&media),
                 GenericRtpPacketizer::defaultClockRate);

            // This is a SendOnly track so only a packetizer is needed for the
            // sent frames.
            auto packetizer = std::make_shared<GenericRtpPacketizer>(rtp_config_);
            track->setMediaHandler(packetizer);

            // Use this track for sending.
            track_send_ = track;
        }
    }

    if (!track_recv_) {
        LOG_ERROR << "Could not setup receive direction video track";
    }

    if (!track_send_) {
        LOG_ERROR << "Could not setup send direction video track";
    }
}

void NetworkHandler::CreatePeerConnection(std::optional<rtc::Description> offer)
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

    if (offer) {
        // The answer is being created in response to the remote peer's offer.
        // Based on the intersection of the local and remote encoder/decoder
        // capabilities, the best codec will be selected for each direction in
        // the answer.

        auto best_decoder = BestDecoderFromDescription(*offer);
        auto best_encoder = BestEncoderFromDescription(*offer);

        // Remove all media tracks from the modified offer.
        offer->clearMedia();

        if (best_decoder == best_encoder) {
            // Add a single video track to handle send/receive directions.
            auto codec_name = ToString(best_decoder.first);
            auto payload_type = best_decoder.second;
            rtc::Description::Video video(codec_name, rtc::Description::Direction::SendRecv);
            video.addVideoCodec(payload_type, codec_name);
            video.addSSRC(kFixedSsrc, codec_name);
            tracks_.emplace_back(peer_->addTrack(video));
            // Add the track to the modified offer.
            offer->addMedia(video);
        } else {
            // Add a video track for the decoder to handle the receive direction.
            {
                auto codec_name = ToString(best_decoder.first);
                auto payload_type = best_decoder.second;
                rtc::Description::Video video(codec_name, rtc::Description::Direction::RecvOnly);
                video.addVideoCodec(payload_type, codec_name);
                video.addSSRC(kFixedSsrc, codec_name);
                tracks_.emplace_back(peer_->addTrack(video));
                // Add the receive track to the modified offer.
                offer->addMedia(video);
            }

            // Add a video track for the encoder to handle the send direction.
            {
                auto codec_name = ToString(best_encoder.first);
                auto payload_type = best_encoder.second;
                rtc::Description::Video video(codec_name, rtc::Description::Direction::SendOnly);
                video.addVideoCodec(payload_type, codec_name);
                video.addSSRC(kFixedSsrc, codec_name);
                tracks_.emplace_back(peer_->addTrack(video));
                // Add the send track to the modified offer.
                offer->addMedia(video);
            }
        }
    } else {
        // The offer is being created. Every codec supported by the local
        // encoder/decoder needs to be added to the offer, and the remote peer
        // will select the actual codecs to be used for each direction based on
        // its local encoder/decoder capabilities.

        // Add a video track for every codec supported by the local encoder
        // and decoder.
        int video_payload_type = 101;
        for (const auto& codec_dir : codec_directions_) {
            auto codec_name = ToString(codec_dir.first);
            auto direction = codec_dir.second;
            rtc::Description::Video video(codec_name, direction);
            video.addVideoCodec(video_payload_type++, codec_name);
            video.addSSRC(kFixedSsrc, codec_name);
            tracks_.emplace_back(peer_->addTrack(video));
            // The track will be implicitly added to the offer.
        }
    }

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

CodecDirections NetworkHandler::GetCodecDirections()
{
    CodecDirections res = {};

    for (auto e : *params_.encoder_codecs) {
        std::pair<VideoCodec, rtc::Description::Direction>
            entry(e, rtc::Description::Direction::SendOnly);

        auto d_begin = params_.decoder_codecs->begin();
        auto d_end = params_.decoder_codecs->end();
        if (std::find(d_begin, d_end, e) != d_end) {
            entry.second = rtc::Description::Direction::SendRecv;
        }

        res.push_back(entry);
    }

    for (auto d : *params_.decoder_codecs) {
        auto found = false;
        for (auto &r : res) {
            if (d == r.first) {
                found = true;
            }
        }

        if (!found) {
            std::pair<VideoCodec, rtc::Description::Direction>
                entry(d, rtc::Description::Direction::RecvOnly);

            res.push_back(entry);
        }
    }

    return res;
}

static bool CanRecv(rtc::Description::Direction dir)
{
    return (dir == rtc::Description::Direction::RecvOnly ||
            dir == rtc::Description::Direction::SendRecv);
}

static bool CanSend(rtc::Description::Direction dir)
{
    return (dir == rtc::Description::Direction::SendOnly ||
            dir == rtc::Description::Direction::SendRecv);
}

std::pair<VideoCodec, int> NetworkHandler::BestDecoderFromDescription(rtc::Description& desc)
{
    for (auto our_codec : *params_.decoder_codecs) {
        for (unsigned int i = 0; i < desc.mediaCount(); ++i) {
            if (std::holds_alternative<rtc::Description::Media*>(desc.media(i))) {
                auto media = std::get<rtc::Description::Media*>(desc.media(i));
                if (media->type() != "video") {
                    continue;
                }

                auto payload_types = media->payloadTypes();
                if (payload_types.size() != 1) {
                    continue;
                }
                auto payload_type = payload_types.front();

                const auto rtp_map = media->rtpMap(payload_type);
                VideoCodec their_codec = FromString(rtp_map->format);
                if (CanRecv(media->direction()) && our_codec == their_codec) {
                    return std::make_pair(our_codec, payload_type);
                }
            }
        }
    }
    return std::make_pair(VideoCodec::UNKNOWN, 0);
}

std::pair<VideoCodec, int> NetworkHandler::BestEncoderFromDescription(rtc::Description& desc)
{
    for (auto our_codec : *params_.encoder_codecs) {
        for (unsigned int i = 0; i < desc.mediaCount(); ++i) {
            if (std::holds_alternative<rtc::Description::Media*>(desc.media(i))) {
                auto media = std::get<rtc::Description::Media*>(desc.media(i));
                if (media->type() != "video") {
                    continue;
                }

                auto payload_types = media->payloadTypes();
                if (payload_types.size() != 1) {
                    continue;
                }
                auto payload_type = payload_types.front();

                const auto rtp_map = media->rtpMap(payload_type);
                VideoCodec their_codec = FromString(rtp_map->format);
                if (CanSend(media->direction()) && our_codec == their_codec) {
                    return std::make_pair(our_codec, payload_type);
                }
            }
        }
    }
    return std::make_pair(VideoCodec::UNKNOWN, 0);
}

} // namespace vacon
