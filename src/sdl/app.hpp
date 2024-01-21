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

#include <cstdint>

#include <SDL3/SDL.h>
#include <imgui.h>

namespace vacon {
namespace sdl {

struct App {
    public:
        struct {
            unsigned    n_preview               = 0;
            unsigned    n_preview_underflow     = 0;
        } stats_;

        bool            enable_my_camera_       = true;
        bool            enable_my_microphone_   = true;
        bool            enable_self_view_       = true;
        bool            enable_stats_overlay_   = false;

        bool            xenable_demo_window_    = false;
        bool            xenable_network_handler_= false;
        bool            xenable_video_handler_  = false;

        float           font_size_sans_         = 12.0f;
        float           font_size_monospace_    = 12.0f;

        ImFont*         font_sans_              = nullptr;
        ImFont*         font_monospace_         = nullptr;

        SDL_Renderer*   sdl_renderer_           = nullptr;
        SDL_Texture*    sdl_texture_preview_    = nullptr;
        SDL_Window*     sdl_window_             = nullptr;

        bool InitSDL();
        void InitImgui();
        void RenderFrame();

    private:
        void CalculateUiSize();
        void RenderPreview();
        void ShowMenu();
        void ShowStatsOverlay(bool*);
};

// Entry point for SDL_RunApp().
int main(int, char **);

} // namespace sdl
} // namespace vacon
