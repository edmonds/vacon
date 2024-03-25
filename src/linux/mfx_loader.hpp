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

#include <memory>

#include <mfx.h>

namespace vacon {
namespace linux {

class MfxLoader {
    public:
        static std::shared_ptr<MfxLoader> GetInstance();
        static void DestroyInstance();
        ~MfxLoader();
        MfxLoader(MfxLoader&& src);
        mfxLoader Get();

    private:
        MfxLoader() = default;
        MfxLoader(mfxLoader ptr) : ptr_(ptr) {};

        mfxLoader ptr_ = nullptr;
};

} // namespace linux
} // namespace vacon
