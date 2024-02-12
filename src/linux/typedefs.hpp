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

#include <readerwritercircularbuffer.h>

namespace vacon {

class PacketRef;

typedef moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<PacketRef>>
    PacketRefQueue;

namespace linux {

class CameraBufferRef;
class DecodedFrame;
class VideoFrame;

typedef moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<CameraBufferRef>>
    CameraBufferQueue;

typedef moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<DecodedFrame>>
    DecodedFrameQueue;

typedef moodycamel::BlockingReaderWriterCircularBuffer<std::shared_ptr<VideoFrame>>
    VideoPacketQueue;

} // namespace linux
} // namespace vacon
