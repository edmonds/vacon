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

#include "linux/font.hpp"

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
    if (auto sans_fname = vacon::linux::GetTrueTypeFileNameByPattern("sans")) {
        g_imfont_sans = io.Fonts->AddFontFromFileTTF((*sans_fname).c_str(), font_size_sans_);
    }
    if (auto mono_fname = vacon::linux::GetTrueTypeFileNameByPattern("monospace")) {
        g_imfont_mono = io.Fonts->AddFontFromFileTTF((*mono_fname).c_str(), font_size_mono_);
    }

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
            ImGui::MenuItem("Mirror self-view", "",     &mirror_self_view_);
            ImGui::Separator();
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
        if (g_imfont_mono) {
            ImGui::PushFont(g_imfont_mono);
        }
        ImGui::Text("Preview frames: %u (%u)", stats_.n_preview, stats_.n_preview_underflow);
        ImGui::Separator();
        ImGui::Text("Display time: %d us", int(1'000'000.0f / io.Framerate));
        ImGui::Text("Display rate: %.3f fps", io.Framerate);
        if (g_imfont_mono) {
            ImGui::PopFont();
        }
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

    if (vh_) {
        ShowPreview();
    }

    ShowMenu();

    if (xenable_demo_window_) {
        ImGui::ShowDemoWindow(&xenable_demo_window_);
    }

    if (enable_stats_overlay_) {
        ShowStatsOverlay(&enable_stats_overlay_);
    }

    if (xenable_network_handler_) {
        StartNetworkHandler();
    } else {
        StopNetworkHandler();
    }

    if (xenable_video_handler_) {
        StartVideoHandler();
    } else {
        StopVideoHandler();
    }

    ImGui::Render();
    SDL_SetRenderScale(sdl_renderer_,
                       io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData());

    SDL_RenderPresent(sdl_renderer_);
}

void App::ShowPreview()
{
    // Get the next preview frame from the camera.
    if (auto cref = vh_->NextPreviewFrame()) {
        LOG_VERBOSE << "NextPreviewFrame() returned buffer index " << cref->buf_.vbuf.index;

        // Save the frame in case it's needed for the next rendering
        // iteration (i.e., if the preview queue underflows).
        preview_cref_ = cref;
    } else {
        // No new preview frame, use the previous frame.
        ++stats_.n_preview_underflow;
    }

    if (enable_self_view_) {
        ++stats_.n_preview;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ShowPreviewWindow();
        ImGui::PopStyleVar(2);
    } else {
        preview_cref_ = nullptr;
    }
}

void App::ShowPreviewWindow()
{
    ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    if (!ImGui::Begin("Self-view", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    if (preview_cref_) {
        // Show the frame from the camera.
        if (mirror_self_view_) {
            ImGui::Image(static_cast<void*>(preview_cref_->buf_.texture),
                         ImVec2(self_view_width_, self_view_height_),
                         ImVec2(1, 0), ImVec2(0, 1));
        } else {
            ImGui::Image(static_cast<void*>(preview_cref_->buf_.texture),
                         ImVec2(self_view_width_, self_view_height_));
        }
    } else {
        // Show the placeholder texture.
        ImGui::Image(static_cast<void*>(sdl_texture_placeholder_),
                     ImVec2(self_view_width_, self_view_height_));
    }

    ImGui::End();
}

void App::ProcessUiEvent(const SDL_Event* event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
}

} // namespace vacon
