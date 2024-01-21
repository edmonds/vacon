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

#include <plog/Log.h>

#define SDL_MAIN_HANDLED
#define SDL_MAIN_USE_CALLBACKS
#include <SDL3/SDL_main.h>
#undef main
#include <SDL3/SDL.h>

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include "common.hpp"
#include "linux/camera_frame.hpp"
#include "linux/video_handler.hpp"
#include "network_handler.hpp"
#include "sdl/texture.hpp"
#include "vacon.hpp"

namespace vacon {
namespace sdl {

struct App app = {};

bool App::InitSDL()
{
    // Initialize SDL.
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        PLOG_FATAL << "SDL_Init() failed: " << SDL_GetError();
        return false;
    }

    // Create window with graphics context.
    Uint32 window_flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_OPENGL;
    sdl_window_ = SDL_CreateWindow("vacon", 1280, 720, window_flags);
    if (!sdl_window_) {
        PLOG_FATAL << "SDL_CreateWindow() failed: " << SDL_GetError();
        return false;
    }

    // Get the renderer associated with the window.
    Uint32 renderer_flags = SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED;
    sdl_renderer_ = SDL_CreateRenderer(sdl_window_, nullptr, renderer_flags);
    if (!sdl_renderer_) {
        PLOG_FATAL << "SDL_CreateRenderer() failed: " << SDL_GetError();
        return false;
    }
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(sdl_renderer_, &info) == 0) {
        PLOG_DEBUG << "Created renderer: " << info.name;
    }

    // Make the window visible.
    if (SDL_ShowWindow(sdl_window_) != 0) {
        PLOG_FATAL << "SDL_ShowWindow() failed: " << SDL_GetError();
        return false;
    }

    // Success.
    return true;
}

void App::InitImgui()
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
    font_sans_ = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/croscore/Arimo-Regular.ttf", font_size_sans_);
    font_monospace_ = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/croscore/Cousine-Regular.ttf", font_size_monospace_);

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
}

void App::CalculateUiSize()
{
    int count = 0;
    if (auto *did = SDL_GetDisplays(&count)) {
        if (auto sdm = SDL_GetDesktopDisplayMode(did[0])) {
            ImGui::GetStyle().ScaleAllSizes(sdm->pixel_density);
            font_size_sans_ = float(int(font_size_sans_ * sdm->pixel_density));
            font_size_monospace_ = float(int(font_size_monospace_ * sdm->pixel_density));
            PLOG_DEBUG << "SDL_DisplayMode.pixel_density = " << sdm->pixel_density;
            PLOG_DEBUG << "Set sans font size to " << font_size_sans_;
            PLOG_DEBUG << "Set monospace font size to " << font_size_monospace_;
        } else {
            PLOG_ERROR << "SDL_GetDesktopDisplayMode() failed: " << SDL_GetError();
        }
        SDL_free(did);
    } else {
        PLOG_ERROR << "SDL_GetDisplays() failed: " << SDL_GetError();
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
        ImGui::PushFont(font_monospace_);
        ImGui::Text("Preview frames: %u (%u)", stats_.n_preview, stats_.n_preview_underflow);
        ImGui::Separator();
        ImGui::Text("Display time: %d us", int(1'000'000.0f / io.Framerate));
        ImGui::Text("Display rate: %.3f fps", io.Framerate);
        ImGui::PopFont();
    }
    ImGui::End();
}

void App::ShowMenu()
{
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("Conference")) {
            if (ImGui::MenuItem("Join")) {
                PLOG_FATAL << "Conference -> Join";
                // TODO
            }
            if (ImGui::MenuItem("Create")) {
                PLOG_FATAL << "Conference -> Create";
                // TODO
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit")) {
                PLOG_FATAL << "Conference -> Quit";

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
                PLOG_FATAL << "Settings -> More settings";
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

void App::RenderPreview()
{
    if (vacon::gApp.vh) {
        auto camera_frame = vacon::gApp.vh->GetNextPreviewFrame();
        if (!camera_frame) {
            ++stats_.n_preview_underflow;
        }

        if (enable_self_view_) {
            RenderPreviewFrame(sdl_renderer_, &sdl_texture_preview_, camera_frame);
            ++stats_.n_preview;
        }
    }
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

    RenderPreview();

    ShowMenu();

    if (xenable_demo_window_) {
        ImGui::ShowDemoWindow(&xenable_demo_window_);
    }

    if (enable_stats_overlay_) {
        ShowStatsOverlay(&enable_stats_overlay_);
    }

    if (xenable_network_handler_) {
        vacon::gApp.StartNetworkHandler();
    } else {
        vacon::gApp.StopNetworkHandler();
    }

    if (xenable_video_handler_) {
        vacon::gApp.StartVideoHandler();
    } else {
        vacon::gApp.StopVideoHandler();
    }

    ImGui::Render();
    SDL_SetRenderScale(sdl_renderer_,
                       io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData());

    SDL_RenderPresent(sdl_renderer_);
}

// AppInit() will be called once before anything else. If this returns 0, the
// app runs. If it returns < 0, the app calls SDL_AppQuit and terminates with
// an exit code that reports an error to the platform. If it returns > 0, the
// app calls AppQuit and terminates with an exit code that reports success to
// the platform. This function should not go into an infinite mainloop; it
// should do any one-time startup it requires and then return.
static int AppInit([[maybe_unused]] int argc,
                   [[maybe_unused]] char **argv)
{
    // Initialize SDL.
    if (!app.InitSDL()) {
        PLOG_FATAL << "Failed to initialize SDL library!";
        return -1;
    }

    // Initialize Dear ImGui.
    app.InitImgui();

    // Success.
    return 0;
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
static int AppIterate(void)
{
    app.RenderFrame();

    if (vacon::gShuttingDown) [[unlikely]] {
        return 1;
    }

    // Success.
    return 0;
}

// AppEvent() will be called whenever an SDL event arrives, on the thread that
// runs AppIterate(). Your app should also not call SDL_PollEvent(),
// SDL_PumpEvent(), etc, as SDL will manage all this for you. Return values are
// the same as from AppIterate(), so you can terminate in response to
// SDL_EVENT_QUIT, etc.
static int AppEvent(const SDL_Event *event)
{
    ImGui_ImplSDL3_ProcessEvent(event);

    if (event->type == SDL_EVENT_QUIT) {
        return 1;
    }

    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        event->window.windowID == SDL_GetWindowID(app.sdl_window_))
    {
        return 1;
    }

    return 0;
}

// AppQuit() is called once before terminating the app--assuming the app isn't
// being forcibly killed or crashed--as a last chance to clean up. After this
// returns, SDL will call SDL_Quit() so the app doesn't have to (but it's safe
// for the app to call it, too). Process termination proceeds as if the app
// returned normally from main(), so atexit handles will run, if your platform
// supports that.
static void AppQuit(void)
{
    PLOG_DEBUG << "Quitting!";

    vacon::gApp.StopVideoHandler();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyTexture(app.sdl_texture_preview_);
    SDL_DestroyRenderer(app.sdl_renderer_);
    SDL_DestroyWindow(app.sdl_window_);
}

int main(int argc, char **argv)
{
    return SDL_EnterAppMainCallbacks(argc, argv, AppInit, AppIterate, AppEvent, AppQuit);
}

} // namespace sdl
} // namespace vacon
