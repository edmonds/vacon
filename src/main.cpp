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

#include <chrono>
#include <cstdlib>
#include <thread>

#include <plog/Log.h>

#define SDL_MAIN_HANDLED
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#undef main
#include <SDL3/SDL.h>

#include "vacon.hpp"
#include "sdl/app.hpp"

using namespace std::chrono_literals;

int main(int argc, char *argv[])
{
    int ret = EXIT_SUCCESS;

    if (!vacon::gApp.Setup(argc, argv)) {
        PLOG_FATAL << "Failed to setup Vacon app!";
        return EXIT_FAILURE;
    }

    if (vacon::gApp.args["--xxx-headless"] == true) {
        while (!vacon::gShuttingDown) {
            std::this_thread::sleep_for(250ms);
        }
    } else {
        PLOG_INFO << "Starting SDL app!";
        ret = SDL_RunApp(0, NULL, vacon::sdl::main, NULL);
    }

    PLOG_INFO << "Shutting down!";
    vacon::gApp.Shutdown();

    return ret;
}
