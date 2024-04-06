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

#include "app.hpp"

#include <cstdlib>

#define SDL_MAIN_HANDLED
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#undef main
#include <SDL3/SDL.h>

// Global instance of the app.
vacon::App gApp = {};

// See:
// https://github.com/libsdl-org/SDL/blob/main/docs/README-main-functions.md.

static int AppInit([[maybe_unused]] void** state, int argc, char **argv)
{
    return gApp.AppInit(argc, argv);
}

static int AppIterate([[maybe_unused]] void* state)
{
    return gApp.AppIterate();
}

static int AppEvent([[maybe_unused]] void* state, const SDL_Event *event)
{
    return gApp.AppEvent(event);
}

static void AppQuit([[maybe_unused]] void* state)
{
    return gApp.AppQuit();
}

int main(int argc, char **argv)
{
    if (getenv("WAYLAND_DISPLAY")) {
        // Prevent SDL3 from incorrectly falling back to Xwayland when native
        // Wayland is available.
        setenv("SDL_VIDEODRIVER", "wayland", 0);
    }

    return SDL_EnterAppMainCallbacks(argc, argv, AppInit, AppIterate, AppEvent, AppQuit);
}
