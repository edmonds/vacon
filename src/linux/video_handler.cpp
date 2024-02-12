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
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <SDL3/SDL.h>
#include <plog/Log.h>

#include "event.hpp"
#include "linux/camera.hpp"
#include "linux/encoder.hpp"
#include "util.hpp"

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

    LOG_INFO << "Waiting for video handler threads to exit...";

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

void VideoHandler::RunCamera(std::stop_token st)
{
    LOG_DEBUG << "Starting Linux camera capture thread ID " << std::this_thread::get_id();
    util::SetThreadName("VCameraCapture");

    PushEvent(Event::CameraStarting);

    // Try multiple times to start the camera capture. This awkwardness is due
    // to real hardware like the Razer Kiyo Pro that sometimes hangs when
    // trying to read the first frame from the kernel.
    bool camera_started = false;
    int n_camera_start_attempts = 3;
    while (n_camera_start_attempts--) {
        if (!camera_) {
            camera_ = Camera::Create(*params_.camera_params);
            if (!camera_) {
                LOG_ERROR << "Camera::Create() failed !!!";
                return;
            }
        }

        if (!camera_->Init()) {
            LOG_ERROR << "Camera::Init() failed !!!";
            camera_ = nullptr;
            continue;
        }

        camera_started = camera_->StartCapturing();
        if (camera_started) {
            break;
        } else {
            LOG_ERROR << "Camera::StartCapturing() failed !!!";
            camera_ = nullptr;
            continue;
        }
    }

    if (!camera_started) {
        LOG_FATAL << "Failed to start capturing frames from camera after multiple attempts, giving up !!!";
        PushEvent(Event::CameraFailed);
        return;
    }

    PushEvent(Event::CameraStarted);

    uint32_t last_sequence = 0;
    while (!st.stop_requested()) {
        // Get the next V4L2 frame from the camera.
        auto cref = camera_->NextFrame();
        if (!cref) {
            continue;
        }

        uint32_t sequence = cref->buf_.vbuf.sequence;
        if (last_sequence > 0 && (sequence != last_sequence + 1)) {
            LOG_DEBUG << std::format("Gap in camera frame sequence, current sequence {}, last sequence {}",
                                     sequence, last_sequence);
        }
        last_sequence = sequence;

        // Enqueue the camera frame onto the encoder queue.
        if (!encoder_queue_.try_enqueue(cref)) {
            LOG_VERBOSE << "Failed to enqueue frame onto encoder queue, discarding!";
        }

        // Enqueue the camera frame onto the preview queue.
        if (!preview_queue_.try_enqueue(cref)) {
            LOG_VERBOSE << "Failed to enqueue frame onto preview queue, discarding!";
        }
    }

    LOG_DEBUG << "Stopping Linux camera capture thread ID " << std::this_thread::get_id();
}

void VideoHandler::RunEncoder(std::stop_token st)
{
    LOG_DEBUG << "Starting video encoder thread ID " << std::this_thread::get_id();

    // Encoder initialization will start a number of background worker threads
    // when libvpl is initialized. Make sure the names of those worker threads
    // are distinct from this thread's name.
    util::SetThreadName("VMfxWorker");

    if (!encoder_->Init()) {
        LOG_ERROR << "Video encoder initialization failed !!!";
        encoder_ = nullptr;
        return;
    }

    util::SetThreadName("VEncoderVideo");

    while (!st.stop_requested()) {
        // Get the next camera frame from the queue.
        std::shared_ptr<CameraBufferRef> cref = nullptr;
        if (encoder_queue_.wait_dequeue_timed(cref, 250ms)) {
            // Encode the camera frame.
            auto video_frame = encoder_->EncodeCameraBuffer(*cref);

            // Get rid of this CameraFrame as soon as possible so the buffer
            // can be re-enqueued to the kernel.
            cref = nullptr;

            if (!video_frame) {
                LOG_ERROR << "Encoder::EncodeCameraFrame() failed!";
                continue;
            }

            // Enqueue the compressed video frame for network transport.
            if (params_.outgoing_video_packet_queue) {
                while (!st.stop_requested()) {
                    if (params_.outgoing_video_packet_queue->wait_enqueue_timed(video_frame, 10ms)) {
                        break;
                    } else {
                        LOG_VERBOSE << "Stalled enqueuing packet onto outgoing video packet queue, retrying";
                    }
                }
            }
        }
    }

    LOG_DEBUG << "Stopping video encoder thread ID " << std::this_thread::get_id();
}

std::shared_ptr<CameraBufferRef> VideoHandler::NextPreviewFrame()
{
    std::shared_ptr<CameraBufferRef> cref = nullptr;
    preview_queue_.try_dequeue(cref);
    return cref;
}

} // namespace linux
} // namespace vacon
