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

#include "app.hpp"

#include <SDL3/SDL.h>
#include <plog/Log.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

namespace vacon {

static ImFont* g_imfont_sans = nullptr;
static ImFont* g_imfont_mono = nullptr;

bool App::InitImgui()
{
    // Initialize ImGui.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // Handle HiDPI adjustments. Must be done before loading fonts.
    CalculateUiSize();

    // Load fonts.
    // XXX: Hardcoded font paths.
    g_imfont_sans = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/croscore/Arimo-Regular.ttf", font_size_sans_);
    g_imfont_mono = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/croscore/Cousine-Regular.ttf", font_size_mono_);

    // Styles.
    ImGui::StyleColorsLight();
    ImGui::GetStyle().FrameBorderSize = 1.0f;
    const ImVec4 bgColor = ImVec4(0.8, 0.8, 0.8, 0.8);
    ImGui::GetStyle().Colors[ImGuiCol_ChildBg] = bgColor;
    ImGui::GetStyle().Colors[ImGuiCol_FrameBg] = bgColor;
    ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg] = bgColor;
    ImGui::GetStyle().Colors[ImGuiCol_PopupBg] = bgColor;
    ImGui::GetStyle().Colors[ImGuiCol_TitleBg] = bgColor;
    ImGui::GetStyle().Colors[ImGuiCol_TitleBgActive] = bgColor;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = bgColor;

    // ImGui/SDL initialization.
    ImGui_ImplSDL3_InitForSDLRenderer(sdl_window_, sdl_renderer_);
    ImGui_ImplSDLRenderer3_Init(sdl_renderer_);

    // Success.
    return true;
}

void App::CalculateUiSize()
{
    int count = 0;
    if (auto *did = SDL_GetDisplays(&count)) {
        if (auto sdm = SDL_GetDesktopDisplayMode(did[0])) {
            ImGui::GetStyle().ScaleAllSizes(sdm->pixel_density);
            font_size_sans_ = float(int(font_size_sans_ * sdm->pixel_density));
            font_size_mono_ = float(int(font_size_mono_ * sdm->pixel_density));
            LOG_DEBUG << "SDL_DisplayMode.pixel_density = " << sdm->pixel_density;
            LOG_DEBUG << "Set sans font size to " << font_size_sans_;
            LOG_DEBUG << "Set monospace font size to " << font_size_mono_;
        } else {
            LOG_ERROR << "SDL_GetDesktopDisplayMode() failed: " << SDL_GetError();
        }
        SDL_free(did);
    } else {
        LOG_ERROR << "SDL_GetDisplays() failed: " << SDL_GetError();
    }
}

void App::ShowMenu()
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Conference")) {
            if (ImGui::MenuItem("Join")) {
                LOG_FATAL << "Conference -> Join";
                // TODO
            }
            if (ImGui::MenuItem("Create")) {
                LOG_FATAL << "Conference -> Create";
                // TODO
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                LOG_FATAL << "Conference -> Quit";

                SDL_Event ev = {};
                ev.type = SDL_EVENT_QUIT;
                ev.quit.type = SDL_EVENT_QUIT;
                ev.quit.timestamp = SDL_GetTicksNS();
                SDL_PushEvent(&ev);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            ImGui::MenuItem("Toggle my camera", "",     &enable_my_camera_);
            ImGui::MenuItem("Toggle my microphone", "", &enable_my_microphone_);
            ImGui::MenuItem("Toggle self-view", "",     &enable_self_view_);
            ImGui::MenuItem("Toggle stats overlay", "", &enable_stats_overlay_);
            ImGui::Separator();
            if (ImGui::MenuItem("More settings")) {
                LOG_FATAL << "Settings -> More settings";
                // TODO
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("XXX")) {
            ImGui::MenuItem("Demo window", "",              &xenable_demo_window_);
            ImGui::MenuItem("Toggle NetworkHandler", "",    &xenable_network_handler_);
            ImGui::MenuItem("Toggle VideoHandler", "",      &xenable_video_handler_);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void App::ShowStatsOverlay(bool* p_open)
{
    if (!*p_open) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    const float PAD = 10.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 work_pos = viewport->WorkPos; // Use work area to avoid menu-bar/task-bar, if any!
    ImVec2 work_size = viewport->WorkSize;
    ImVec2 window_pos, window_pos_pivot;
    window_pos.x = work_pos.x + work_size.x - PAD;
    window_pos.y = work_pos.y + work_size.y - PAD;
    window_pos_pivot.x = 1.0f;
    window_pos_pivot.y = 1.0f;
    ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);

    ImGui::SetNextWindowBgAlpha(0.80f); // Transparent background
    if (ImGui::Begin("Stats", p_open, window_flags)) {
        ImGui::PushFont(g_imfont_mono);
        ImGui::Text("Preview frames: %u (%u)", stats_.n_preview, stats_.n_preview_underflow);
        ImGui::Separator();
        ImGui::Text("Display time: %d us", int(1'000'000.0f / io.Framerate));
        ImGui::Text("Display rate: %.3f fps", io.Framerate);
        ImGui::PopFont();
    }
    ImGui::End();
}

void App::RenderFrame()
{
    // Start the Dear ImGui frame.
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();

    // Fill the window with the background color.
    SDL_SetRenderDrawColor(sdl_renderer_, 58, 110, 165, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(sdl_renderer_);

#if 0
    auto bref = camera_->NextFrame();
    if (!bref) {
        LOG_DEBUG << "Didn't get a frame :-(";
        return 0;
    }

    if (SDL_RenderTextureRotated(sdl_renderer_,
                                 bref->buf_.texture,
                                 nullptr,   /* srcrect */
                                 nullptr,   /* dstrect */
                                 0.0,       /* angle */
                                 nullptr,   /* center */
                                 sdl_renderer_flip_) != 0)
    {
        LOG_FATAL << "SDL_RenderTextureRotated() failed: " << SDL_GetError();
    }

    SDL_RenderPresent(sdl_renderer_);
#endif

#if 0
    // Render preview frame.
    if (vacon::gApp.vh) {
        SDL_Texture *texture = last_preview_v4l2_texture_;

        // Get the next preview frame from the camera.
        if (auto camera_frame = vacon::gApp.vh->GetNextPreviewFrame()) {
            auto index = camera_frame->expbuf_.index;
            PLOG_VERBOSE << "GetNextPreviewFrame() returned expbuf index " << index;

            // Look up the texture that corresponds to this V4L2 buffer index.
            texture = preview_v4l2_textures_.at(index);

            // Save the frame in case it's needed for the next rendering iteration.
            preview_v4l2_frame_ = camera_frame;
        } else {
            // No new preview frame, use the previous frame.
            ++stats_.n_preview_underflow;
            PLOG_FATAL << "Didn't get a frame :-(";
        }

        if (texture && enable_self_view_) {
            ++stats_.n_preview;
            if (SDL_RenderTextureRotated(sdl_renderer_, texture, nullptr, nullptr, 0.0, nullptr, SDL_FLIP_HORIZONTAL) != 0) {
                PLOG_ERROR << "SDL_RenderTextureRotated() failed: " << SDL_GetError();
            }
            last_preview_v4l2_texture_ = texture;
        }
    }
#endif

    ShowMenu();

    if (xenable_demo_window_) {
        ImGui::ShowDemoWindow(&xenable_demo_window_);
    }

    if (enable_stats_overlay_) {
        ShowStatsOverlay(&enable_stats_overlay_);
    }

#if 0
    if (xenable_network_handler_) {
        vacon::gApp.StartNetworkHandler();
    } else {
        vacon::gApp.StopNetworkHandler();
    }

    if (xenable_video_handler_) {
        StartVideoHandler();
    } else {
        StopVideoHandler();
    }
#endif

    ImGui::Render();
    SDL_SetRenderScale(sdl_renderer_,
                       io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData());

    SDL_RenderPresent(sdl_renderer_);
}

void App::ProcessUiEvent(const SDL_Event* event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
}

} // namespace vacon
