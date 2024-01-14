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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <readerwritercircularbuffer.h>

#include "../common.hpp"
#include "camera.hpp"
#include "encoder.hpp"

namespace vacon {
namespace linux {

struct VideoHandlerParams {
    std::optional<CameraParams> camera_params = std::nullopt;
    std::optional<EncoderParams> encoder_params = std::nullopt;
    std::shared_ptr
        <moodycamel::BlockingReaderWriterCircularBuffer
            <std::shared_ptr
                <linux::VideoFrame> > >
        outgoing_video_packet_queue = nullptr;
};

class VideoHandler {
    public:
        static std::unique_ptr<VideoHandler> Create(const VideoHandlerParams&);
        VideoHandler(VideoHandler&&) = default;
        ~VideoHandler();
        void Init();
        void Stop();
        void Join();

    private:
        VideoHandler() = default;
        void RunCamera(std::stop_token);
        void RunEncoder(std::stop_token);

        VideoHandlerParams params_;

        std::vector<std::jthread>   threads_ = {};
        std::shared_ptr<Camera>     camera_ = {};
        std::shared_ptr<Encoder>    encoder_ = {};

        moodycamel::BlockingReaderWriterCircularBuffer<CameraFrame*>
            camera_frame_queue_ = moodycamel::BlockingReaderWriterCircularBuffer<CameraFrame*>(2);
};

} // namespace linux
} // namespace vacon
