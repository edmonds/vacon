// Copyright (c) 2024 The Vacon Authors
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

#include "video_handler.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <readerwritercircularbuffer.h>

#include "../common.hpp"
#include "camera.hpp"
#include "encoder.hpp"

using namespace std::chrono_literals;

namespace vacon {
namespace linux {

std::unique_ptr<VideoHandler> VideoHandler::Create(const VideoHandlerParams& params)

{
    auto vh = std::make_unique<VideoHandler>(VideoHandler {});
    vh->params_ = params;

    if (params.camera_params) {
        vh->camera_ = Camera::Create(*params.camera_params);
        if (!vh->camera_) {
            return nullptr;
        }
    }

    if (params.encoder_params) {
        vh->encoder_ = Encoder::Create(*params.encoder_params);
        if (!vh->encoder_) {
            return nullptr;
        }
    }

    return vh;
}

VideoHandler::~VideoHandler()
{
    if (threads_.size() == 0) {
        return;
    }

    Stop();
    Join();

    // Drain any video frames remaining on the outgoing video packet queue, so
    // that ~VideoFrame() never runs after ~Encoder().
    if (params_.outgoing_video_packet_queue) {
        std::shared_ptr<linux::VideoFrame> frame = {};
        while (params_.outgoing_video_packet_queue->wait_dequeue_timed(frame, 10ms)) {
            PLOG_VERBOSE << fmt::format("Drained VideoFrame @ {}", fmt::ptr(frame.get()));
        }
    }
}

void VideoHandler::Init()
{
    if (camera_) {
        threads_.emplace_back(std::jthread { [&](std::stop_token st) { RunCamera(st); } });
    }

    if (encoder_) {
        threads_.emplace_back(std::jthread { [&](std::stop_token st) { RunEncoder(st); } });
    }
}

void VideoHandler::Stop()
{
    for (auto& thread : threads_) {
        thread.request_stop();
    }
    threads_.clear();
}

void VideoHandler::Join()
{
    PLOG_INFO << "Waiting for video handler threads to exit...";

    for (auto& thread : threads_) {
        if (thread.joinable()) {
            PLOG_DEBUG << "Trying to join thread ID " << thread.get_id();
            thread.join();
        } else {
            PLOG_FATAL << "Thread ID " << thread.get_id() << " is not joinable ?!";
        }
    }
}

void VideoHandler::RunCamera(std::stop_token st)
{
    PLOG_DEBUG << "Starting Linux camera capture thread ID " << std::this_thread::get_id();
    setThreadName("VCameraCapture");

    // Try multiple times to start the camera capture. This awkwardness is due
    // to real hardware like the Razer Kiyo Pro that sometimes hangs when
    // trying to read the first frame from the kernel.
    bool camera_started = false;
    int n_camera_start_attempts = 3;
    while (n_camera_start_attempts--) {
        if (!camera_) {
            camera_ = Camera::Create(*params_.camera_params);
            if (!camera_) {
                PLOG_ERROR << "Camera::Create() failed !!!";
                return;
            }
        }

        if (!camera_->Init()) {
            PLOG_ERROR << "Camera::Init() failed !!!";
            camera_ = nullptr;
            continue;
        }

        camera_started = camera_->StartCapturing();
        if (camera_started) {
            break;
        } else {
            PLOG_ERROR << "Camera::StartCapturing() failed !!!";
            camera_ = nullptr;
            continue;
        }
    }

    if (!camera_started) {
        PLOG_FATAL << "Failed to start capturing frames from camera after multiple attempts, giving up !!!";
        return;
    }

    uint32_t last_sequence = 0;
    while (!st.stop_requested()) {
        // Get the next V4L2 frame from the camera.
        auto frame = camera_->ReadFrame();
        if (!frame) {
            continue;
        }

        uint32_t sequence = frame->buf_.sequence;
        if (last_sequence > 0 && (sequence != last_sequence + 1)) {
            PLOG_INFO << fmt::format("Gap in camera frame sequence, current sequence {}, last sequence {}",
                                     sequence, last_sequence);
        }
        last_sequence = sequence;

        // Enqueue the camera frame onto the encoder queue.
        if (!encoder_queue_.try_enqueue(frame)) {
            PLOG_VERBOSE << "Failed to enqueue frame onto encoder queue, discarding!";
        }

        // Enqueue the camera frame onto the preview queue.
        if (!preview_queue_.try_enqueue(frame)) {
            PLOG_VERBOSE << "Failed to enqueue frame onto preview queue, discarding!";
        }
    }

    PLOG_DEBUG << "Stopping Linux camera capture thread ID " << std::this_thread::get_id();
}

void VideoHandler::RunEncoder(std::stop_token st)
{
    PLOG_DEBUG << "Starting video encoder thread ID " << std::this_thread::get_id();

    // Encoder initialization will start a number of background worker threads
    // when libvpl is initialized. Make sure the names of those worker threads
    // are distinct from this thread's name.
    setThreadName("VMfxWorker");

    if (!encoder_->Init()) {
        PLOG_ERROR << "Video encoder initialization failed !!!";
        encoder_ = nullptr;
        return;
    }

    setThreadName("VEncoderVideo");

    while (!st.stop_requested()) {
        // Get the next camera frame from the queue.
        std::shared_ptr<CameraFrame> camera_frame = nullptr;
        if (encoder_queue_.wait_dequeue_timed(camera_frame, 250ms)) {
            // Encode the camera frame.
            auto video_frame = encoder_->EncodeCameraFrame(*camera_frame);

            // Get rid of this CameraFrame as soon as possible so the buffer
            // can be re-enqueued to the kernel.
            camera_frame = nullptr;

            if (!video_frame) {
                PLOG_ERROR << "Encoder::EncodeCameraFrame() failed!";
                continue;
            }

            // Enqueue the compressed video frame for network transport.
            if (params_.outgoing_video_packet_queue) {
                while (!st.stop_requested()) {
                    if (params_.outgoing_video_packet_queue->wait_enqueue_timed(video_frame, 10ms)) {
                        break;
                    } else {
                        PLOG_VERBOSE << "Stalled enqueuing packet onto outgoing video packet queue, retrying";
                    }
                }
            }
        }
    }

    PLOG_DEBUG << "Stopping video encoder thread ID " << std::this_thread::get_id();
}

std::shared_ptr<CameraFrame> VideoHandler::GetNextPreviewFrame()
{
    std::shared_ptr<CameraFrame> camera_frame = nullptr;
    preview_queue_.try_dequeue(camera_frame);
    return camera_frame;
}

} // namespace linux
} // namespace vacon
