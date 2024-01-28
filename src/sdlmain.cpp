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

#define SDL_MAIN_HANDLED
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#undef main
#include <SDL3/SDL.h>

// Global instance of the app.
struct vacon::App app = {};

// Documentation below derived from:
// https://github.com/libsdl-org/SDL/blob/main/docs/README-main-functions.md.

// Simple DirectMedia Layer
// Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.

// AppInit() will be called once before anything else. If this returns 0, the
// app runs. If it returns < 0, the app calls SDL_AppQuit and terminates with
// an exit code that reports an error to the platform. If it returns > 0, the
// app calls AppQuit and terminates with an exit code that reports success to
// the platform. This function should not go into an infinite mainloop; it
// should do any one-time startup it requires and then return.
static int AppInit(int argc, char **argv)
{
    return app.AppInit(argc, argv);
}

// AppIterate() is called over and over, possibly at the refresh rate of the
// display or some other metric that the platform dictates. This is where the
// heart of your app runs. It should return as quickly as reasonably possible,
// but it's not a "run one memcpy and that's all the time you have" sort of
// thing. The app should do any game updates, and render a frame of video. If
// it returns < 0, SDL will call AppQuit() and terminate the process with an
// exit code that reports an error to the platform. If it returns > 0, the app
// calls AppQuit() and terminates with an exit code that reports success to the
// platform. If it returns 0, then AppIterate() will be called again at some
// regular frequency. The platform may choose to run this more or less (perhaps
// less in the background, etc), or it might just call this function in a loop
// as fast as possible. You do not check the event queue in this function
// (AppEvent() exists for that).
static int AppIterate()
{
    return app.AppIterate();
}

// AppEvent() will be called whenever an SDL event arrives, on the thread that
// runs AppIterate(). Your app should also not call SDL_PollEvent(),
// SDL_PumpEvent(), etc, as SDL will manage all this for you. Return values are
// the same as from AppIterate(), so you can terminate in response to
// SDL_EVENT_QUIT, etc.
static int AppEvent(const SDL_Event *event)
{
    return app.AppEvent(event);
}

// AppQuit() is called once before terminating the app--assuming the app isn't
// being forcibly killed or crashed--as a last chance to clean up. After this
// returns, SDL will call SDL_Quit() so the app doesn't have to (but it's safe
// for the app to call it, too). Process termination proceeds as if the app
// returned normally from main(), so atexit handles will run, if your platform
// supports that.
static void AppQuit()
{
    return app.AppQuit();
}

int main(int argc, char **argv)
{
    return SDL_EnterAppMainCallbacks(argc, argv, AppInit, AppIterate, AppEvent, AppQuit);
}
