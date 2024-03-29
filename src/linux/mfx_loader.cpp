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

#include "linux/mfx_loader.hpp"

#include <format>
#include <memory>
#include <mutex>

#include <mfx.h>
#include <plog/Log.h>

namespace vacon {
namespace linux {

static std::mutex mutex;
static std::shared_ptr<MfxLoader> instance = nullptr;

std::shared_ptr<MfxLoader> MfxLoader::GetInstance()
{
    std::lock_guard lock(mutex);

    if (!instance) {
        auto ptr = MFXLoad();
        if (!ptr) {
            return nullptr;
        }
        LOG_DEBUG << std::format("Created MFX loader @ {}", (void*)ptr);
        instance = std::make_shared<MfxLoader>(MfxLoader(ptr));
    }

    return instance;
}

void MfxLoader::DestroyInstance()
{
    instance = nullptr;
}

MfxLoader::~MfxLoader()
{
    if (ptr_) {
        LOG_VERBOSE << std::format("Destroying MFX loader @ {}", (void*)ptr_);
        MFXUnload(ptr_);
        ptr_ = nullptr;
    }
}

MfxLoader::MfxLoader(MfxLoader&& src)
{
    ptr_ = src.ptr_;
    src.ptr_ = nullptr;
}

mfxLoader MfxLoader::Get()
{
    return ptr_;
}

} // namespace linux
} // namespace vacon
