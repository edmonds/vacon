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

#include <SDL3/SDL.h>

#include "linux/camera_frame.hpp"

namespace vacon {
namespace sdl {

void RenderPreviewFrame(SDL_Renderer* renderer,
                        SDL_Texture** texture,
                        std::shared_ptr<linux::CameraFrame> frame);

} // namespace sdl
} // namespace vacon
