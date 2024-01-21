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

#include <cstdint>
#include <memory>

#include <SDL3/SDL.h>

#include "common.hpp"
#include "linux/camera_frame.hpp"

namespace vacon {
namespace sdl {

static Uint32 GetSDLPixelFormatFromV4L2PixelFormat(uint32_t v4l2_pix_fmt)
{
    switch (v4l2_pix_fmt) {
    case V4L2_PIX_FMT_NV12:
        return SDL_PIXELFORMAT_NV12;
    case V4L2_PIX_FMT_UYVY:
        return SDL_PIXELFORMAT_UYVY;
    case V4L2_PIX_FMT_YUYV:
        return SDL_PIXELFORMAT_YUY2;
    default:
        return SDL_PIXELFORMAT_UNKNOWN;
    }
}

static bool GetTextureForMemoryFrame(SDL_Renderer* renderer,
                                     SDL_Texture** texture,
                                     std::shared_ptr<linux::CameraFrame> frame)
{
    int texture_width = 0, texture_height = 0;
    Uint32 texture_format = SDL_PIXELFORMAT_UNKNOWN;

    struct v4l2_pix_format fmt = frame->Fmt();
    Uint32 frame_format = GetSDLPixelFormatFromV4L2PixelFormat(fmt.pixelformat);
    if (frame_format == SDL_PIXELFORMAT_UNKNOWN) {
        return false;
    }

    if (*texture && SDL_QueryTexture(*texture, &texture_format, NULL, &texture_width, &texture_height) != 0) {
        PLOG_ERROR << "SDL_QueryTexture() failed: " << SDL_GetError();
        return false;
    }

    if (!*texture
        || (uint32_t)texture_width != fmt.width
        || (uint32_t)texture_height != fmt.height
        || texture_format != frame_format)
    {
        if (*texture) {
            PLOG_INFO << fmt::format("Destroying SDL_Texture @ {} !!!", fmt::ptr(*texture));
            SDL_DestroyTexture(*texture);
        }

        *texture = SDL_CreateTexture(renderer, frame_format, SDL_TEXTUREACCESS_STREAMING, fmt.width, fmt.height);
        if (!*texture) {
            PLOG_ERROR << "SDL_CreateTexture() failed: " << SDL_GetError();
            return false;
        }

        SDL_SetTextureBlendMode(*texture, SDL_BLENDMODE_NONE);
        SDL_SetTextureScaleMode(*texture, SDL_SCALEMODE_LINEAR);
    }

    if (SDL_UpdateTexture(*texture, NULL, frame->data_, fmt.bytesperline) != 0) {
        PLOG_ERROR << "SDL_UpdateTexture() failed: " << SDL_GetError();
    }

    return true;
}

void RenderPreviewFrame(SDL_Renderer* renderer,
                        SDL_Texture** texture,
                        std::shared_ptr<linux::CameraFrame> frame)
{
    if (frame && !GetTextureForMemoryFrame(renderer, texture, frame)) {
        PLOG_ERROR << "Couldn't get texture for frame: " << SDL_GetError();
    }

    if (*texture && SDL_RenderTextureRotated(renderer, *texture, NULL, NULL, 0.0, NULL, SDL_FLIP_HORIZONTAL) != 0) {
        PLOG_ERROR << "SDL_RenderTextureRotated() failed: " << SDL_GetError();
    }
}

} // namespace sdl
} // namespace vacon
