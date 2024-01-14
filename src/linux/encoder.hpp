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
#include <string>

#include <mfx.h>

#include "../common.hpp"
#include "camera_frame.hpp"
#include "video_frame.hpp"

namespace vacon {
namespace linux {

struct EncoderParams {
    std::string input_pixel_format;
    int width;
    int height;
    int frame_rate;
    int bitrate_kbps;
};

class Encoder {
    public:
        static std::shared_ptr<Encoder> Create(const EncoderParams&);
        Encoder(Encoder&&) = default;
        ~Encoder();
        bool Init();

        std::shared_ptr<VideoFrame> EncodeCameraFrame(CameraFrame&);

    private:
        Encoder() = default;

        bool InitMfxVideoParamEncode();
        bool InitLibraryEncode();

        EncoderParams       params_;
        mfxLoader           mfx_loader_ = nullptr;
        mfxSession          mfx_session_encode_ = nullptr;
        mfxVideoParam       mfx_videoparam_encode_ = {};
        mfxExtCodingOption2 mfx_eco2_ = {};
        mfxExtCodingOption3 mfx_eco3_ = {};
};

} // namespace linux
} // namespace vacon
