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

#include <SDL3/SDL.h>
#include <plog/Log.h>

#include "event.hpp"

namespace vacon {

void PushEvent(Event event_code)
{
    auto event = SDL_Event {
        .user = {
            .type = SDL_EVENT_USER,
            .reserved = 0,
            .timestamp = 0,
            .windowID = 0,
            .code = static_cast<Sint32>(event_code),
            .data1 = nullptr,
            .data2 = nullptr,
        }
    };

    if (SDL_PushEvent(&event) < 0) {
        LOG_DEBUG << "SDL_PushEvent() failed: " << SDL_GetError();
    }
}

} // namespace vacon
