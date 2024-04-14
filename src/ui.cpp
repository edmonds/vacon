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

#include <chrono>

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
    const ImVec4 disabledColor = ImVec4(0.3, 0.3, 0.3, 1.0);
    ImGui::GetStyle().Colors[ImGuiCol_TextDisabled] = disabledColor;

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
            if (ImGui::MenuItem("Create", "Ctrl+N")) {
                LOG_INFO << "Conference -> Create";
                CreateConference();
            }
            if (ImGui::MenuItem("Join")) {
                LOG_FATAL << "Conference -> Join";
                // TODO
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Copy invite to clipboard", "Ctrl+C", false, invite_ != nullptr)) {
                LOG_INFO << "Conference -> Copy invite to clipboard";
                CopyInviteToClipboard();
            }
            if (ImGui::MenuItem("Join from clipboard invite", "Ctrl+V")) {
                LOG_INFO << "Conference -> Join from clipboard invite";
                JoinConferenceFromClipboard();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Shift+Q")) {
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

        ImGui::Text("Draw: %.3f fps", io.Framerate);

        if (nh_) {
            auto s = nh_->s_recv_fps_.Result();
            ImGui::Text("Recv: %.3f ± %.2f fps [%.1f, %.1f]", s.mean, s.stdev, s.min, s.max);
        }

        if (nh_) {
            auto s = nh_->s_send_fps_.Result();
            ImGui::Text("Send: %.3f ± %.2f fps [%.1f, %.1f]", s.mean, s.stdev, s.min, s.max);
        }

        ImGui::Separator();

        if (!camera_format_str_.empty()) {
            ImGui::Text("Camera format:  %s", camera_format_str_.c_str());
        }
        if (!decoder_codec_str_.empty()) {
            ImGui::Text("Decoder codec:  %s", decoder_codec_str_.c_str());
        }
        if (!encoder_codec_str_.empty()) {
            ImGui::Text("Encoder codec:  %s", encoder_codec_str_.c_str());
        }

        ImGui::Text("Camera frames:  %zu (M:%zu, OE:%zu, OP:%zu)",
                    linux::n_frames_camera_success          .load(std::memory_order_relaxed),
                    linux::n_frames_camera_missed           .load(std::memory_order_relaxed),
                    linux::n_frames_camera_overflow_encoder .load(std::memory_order_relaxed),
                    linux::n_frames_camera_overflow_preview .load(std::memory_order_relaxed)
        );
        ImGui::Text("Decoded frames: %zu (F:%zu, O:%zu)",
                    linux::n_frames_decode_success  .load(std::memory_order_relaxed),
                    linux::n_frames_decode_fail     .load(std::memory_order_relaxed),
                    linux::n_frames_decode_overflow .load(std::memory_order_relaxed)
        );
        ImGui::Text("Encoded frames: %zu (F:%zu, S:%zu)",
                    linux::n_frames_encode_success  .load(std::memory_order_relaxed),
                    linux::n_frames_encode_fail     .load(std::memory_order_relaxed),
                    linux::n_frames_encode_stall    .load(std::memory_order_relaxed)
        );
        ImGui::Text("Preview frames: %u (U:%u)", stats_.n_preview, stats_.n_preview_underflow);
        ImGui::Text("Remote frames:  %u (U:%u)", stats_.n_remote, stats_.n_remote_underflow);

        if (encoder_) {
            ImGui::Separator();

            auto s = encoder_->s_encode_size_.Result();
            ImGui::Text("Encoded frame: %d ± %d KB [%d, %d]",
                        (int)(s.mean/1024.0), (int)(s.stdev/1024.0),
                        (int)(s.min/1024.0), (int)(s.max/1024.0));
        }

        ImGui::Separator();

        if (camera_) {
            auto s = camera_->s_capture_time_.Result();
            ImGui::Text("Camera: %d ± %d µs [%d, %d]", (int)s.mean, (int)s.stdev, (int)s.min, (int)s.max);
        }

        if (decoder_) {
            auto s = decoder_->s_decode_time_.Result();
            ImGui::Text("Decode: %d ± %d µs [%d, %d]", (int)s.mean, (int)s.stdev, (int)s.min, (int)s.max);
        }

        if (encoder_) {
            auto s = encoder_->s_encode_time_.Result();
            ImGui::Text("Encode: %d ± %d µs [%d, %d]", (int)s.mean, (int)s.stdev, (int)s.min, (int)s.max);
        }

        {
            auto s = s_render_time_.Result();
            ImGui::Text("Render: %d ± %d µs [%d, %d]", (int)s.mean, (int)s.stdev, (int)s.min, (int)s.max);
        }

        {
            auto s = s_present_time_.Result();
            ImGui::Text("Present: %d ± %d µs [%d, %d]", (int)s.mean, (int)s.stdev, (int)s.min, (int)s.max);
        }

        {
            auto s = s_display_time_.Result();
            ImGui::Text("Display: %d ± %d µs [%d, %d]", (int)s.mean, (int)s.stdev, (int)s.min, (int)s.max);
        }

        if (g_imfont_mono) {
            ImGui::PopFont();
        }
    }
    ImGui::End();
}

void App::RenderFrame()
{
    auto t_start = std::chrono::steady_clock::now();

    // Start the Dear ImGui frame.
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    s_display_time_.Update(1'000'000.0f / io.Framerate);

    // Fill the window with the background color.
    SDL_SetRenderDrawColor(sdl_renderer_, 58, 110, 165, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(sdl_renderer_);

    ShowDecodedVideoFrame();

    if (camera_) {
        ShowPreview();
    }

    ShowMenu();

    if (enable_stats_overlay_) {
        ShowStatsOverlay(&enable_stats_overlay_);
    }

    if (xxx_enable_imgui_demo_window_) {
        ImGui::ShowDemoWindow(&xxx_enable_imgui_demo_window_);
    }

    ImGui::Render();
    SDL_SetRenderScale(sdl_renderer_,
                       io.DisplayFramebufferScale.x,
                       io.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData());

    auto t_render = std::chrono::steady_clock::now();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(t_render - t_start).count();
    s_render_time_.Update(micros);

    SDL_RenderPresent(sdl_renderer_);

    auto t_present = std::chrono::steady_clock::now();
    micros = std::chrono::duration_cast<std::chrono::microseconds>(t_present - t_render).count();
    s_present_time_.Update(micros);
}

void App::ShowDecodedVideoFrame()
{
    // Get the next decoded video frame from the decoder.
    if (decoded_video_frame_queue_->try_dequeue(decoded_frame_)) {
        ++stats_.n_remote;
    } else {
        // No new video frame available from the decoder.
        if (decoded_frame_) [[likely]] {
            ++stats_.n_remote_underflow;
        } else {
            // No previous frame, either.
            return;
        }
    }

    // Export the decoded video frame to an OpenGL texture.
    if (!decoded_frame_->texture_) {
        if (!decoded_frame_->ExportToOpenGL(sdl_renderer_)) {
            LOG_ERROR << "DecodedFrame::ExportToOpenGL() failed";
            decoded_frame_ = nullptr;
            return;
        }
    }

    // Render the OpenGL texture.
    if (SDL_RenderTexture(sdl_renderer_,
                          decoded_frame_->texture_,
                          nullptr /* srcrect */,
                          nullptr /* dstrect */))
    {
        LOG_ERROR << "SDL_RenderTexture() failed: " << SDL_GetError();
    }
}

void App::ShowPreview()
{
    // Get the next preview frame from the camera.
    std::shared_ptr<linux::CameraBufferRef> cref = nullptr;
    if (preview_queue_->try_dequeue(cref)) {
        LOG_VERBOSE << "NextPreviewFrame() returned buffer index " << cref->buf_.vbuf.index;

        // Save the frame in case it's needed for the next rendering
        // iteration (i.e., if the preview queue underflows).
        preview_cref_ = cref;
    } else {
        // No new preview frame, use the previous frame if available.
        if (preview_cref_) [[likely]] {
            ++stats_.n_preview_underflow;
        }
    }

    if (enable_self_view_) {
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
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    if (!ImGui::Begin("Self-view", nullptr, window_flags)) {
        ImGui::End();
        return;
    }

    if (preview_cref_) {
        ++stats_.n_preview;

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

    if (ImGui::IsItemClicked(1 /* right mouse button */)) {
        switch (self_view_width_) {
        case 128: self_view_width_ = 256; self_view_height_ = 144; break;
        case 256: self_view_width_ = 384; self_view_height_ = 216; break;
        case 384: self_view_width_ = 512; self_view_height_ = 288; break;
        case 512: self_view_width_ = 640; self_view_height_ = 360; break;
        case 640: self_view_width_ = 128; self_view_height_ = 72; break;
        }
    }

    ImGui::End();
}

void App::ProcessUiEvent(const SDL_Event* event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
}

} // namespace vacon
