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

#include <format>
#include <set>

#include <SDL3/SDL.h>
#include <SDL3/SDL_egl.h>
#include <SDL3/SDL_opengl.h>
#include <plog/Log.h>

namespace vacon {

bool App::InitSDL()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOG_FATAL << "SDL_Init() failed: " << SDL_GetError();
        return false;
    }

    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");

    SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_OPENGL;
    sdl_window_ = SDL_CreateWindow(PROJECT_NAME, 1920, 1080, flags);
    if (!sdl_window_) {
        LOG_FATAL << "SDL_CreateWindow() failed: " << SDL_GetError();
        return false;
    }

    if (!InitSDLRenderer()) {
        LOG_FATAL << "InitSDLRenderer() failed";
        return false;
    }

    if (!InitSDLTextures()) {
        LOG_FATAL << "InitSDLTextures() failed";
        return false;
    }

    if (SDL_ShowWindow(sdl_window_) != 0) {
        LOG_FATAL << "SDL_ShowWindow() failed: " << SDL_GetError();
        return -1;
    }

    // Success.
    return true;
}

bool App::InitSDLRenderer()
{
    // Get the renderer associated with the window.
    Uint32 renderer_flags = SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED;
    sdl_renderer_ = SDL_CreateRenderer(sdl_window_, nullptr, renderer_flags);
    if (!sdl_renderer_) {
        LOG_FATAL << "SDL_CreateRenderer() failed: " << SDL_GetError();
        return false;
    }

    // Check if the opengles2 renderer was really used.
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(sdl_renderer_, &info) == 0) {
        LOG_DEBUG << "Created renderer: " << info.name;
        if (std::string(info.name) != "opengles2") {
            LOG_FATAL << std::format("SDL didn't create an opengles2 renderer, used {} instead", info.name);
            return false;
        }

        // Log the pixel formats that the renderer supports.
        for (int i = 0; i < info.num_texture_formats; ++i) {
            auto pixel_format = info.texture_formats[i];
            const char *pixel_format_name = SDL_GetPixelFormatName(pixel_format);
            LOG_VERBOSE << "Renderer supports texture pixel format: " << pixel_format_name;
        }
    }

    // Check for the EGL extensions "EGL_EXT_image_dma_buf_import" and
    // "EGL_EXT_image_dma_buf_import_modifiers".
    const char *egl_query_extensions = eglQueryString(eglGetCurrentDisplay(), EGL_EXTENSIONS);
    if (egl_query_extensions) {
        LOG_VERBOSE << "Supported EGL extensions: " << egl_query_extensions;

        std::string extensions(egl_query_extensions);

        std::vector<std::string> wanted_extensions{
            "EGL_EXT_image_dma_buf_import",
            "EGL_EXT_image_dma_buf_import_modifiers",
        };

        for (auto& wanted : wanted_extensions) {
            if (extensions.contains(wanted)) {
                LOG_VERBOSE << "Required EGL extension '" << wanted << "' is supported";
            } else {
                LOG_FATAL << "Required EGL extension '" << wanted << "' is not supported";
                return false;
            }
        }
    } else {
        LOG_FATAL << std::format("eglQueryString(EGL_EXTENSIONS) failed with error code {:#010x}", eglGetError());
        return false;
    }

    // Check for the OpenGL extension "GL_OES_EGL_image_external".
    GLint n_extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n_extensions);
    if (n_extensions > 0) {
        std::set<std::string> extensions;
        for (GLint i = 0; i < n_extensions; ++i) {
            extensions.insert(reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, i)));
        }
        std::string wanted("GL_OES_EGL_image_external");
        if (extensions.find(wanted) != extensions.end()) {
            LOG_VERBOSE << "Required OpenGL extension '" << wanted << "' is supported";
        } else {
            LOG_FATAL << "Required OpenGL extension '" << wanted << "' is not supported";
            return false;
        }
    }

    // Success.
    return true;
}

bool App::InitSDLTextures()
{
    sdl_texture_placeholder_ =
        SDL_CreateTexture(sdl_renderer_,
                          SDL_PIXELFORMAT_RGBA8888,
                          SDL_TEXTUREACCESS_TARGET,
                          8 /* width */,
                          8 /* height */);
    if (!sdl_texture_placeholder_) {
        LOG_FATAL << "SDL_CreateTexture() failed: " << SDL_GetError();
        return false;
    }

    if (SDL_SetRenderTarget(sdl_renderer_, sdl_texture_placeholder_) != 0) {
        LOG_FATAL << "SDL_SetRenderTarget() failed: " << SDL_GetError();
        return false;
    }

    // Magenta.
    if (SDL_SetRenderDrawColor(sdl_renderer_,
                               255  /* r */,
                               0    /* g */,
                               255  /* b */,
                               0    /* a */) != 0)
    {
        LOG_FATAL << "SDL_SetRenderDrawColor() failed: " << SDL_GetError();
        return false;
    }

    if (SDL_RenderClear(sdl_renderer_) != 0) {
        LOG_FATAL << "SDL_RenderClear() failed: " << SDL_GetError();
        return false;
    }

    if (SDL_SetRenderTarget(sdl_renderer_, nullptr) != 0) {
        LOG_FATAL << "SDL_SetRenderTarget() failed: " << SDL_GetError();
        return false;
    }

    // Success.
    return true;
}

} // namespace vacon
