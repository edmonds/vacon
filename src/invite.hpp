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
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <hydrogen.h>
#include <nlohmann/json_fwd.hpp>

namespace vacon {

struct InviteParams {
    std::string signaling_server;
    std::string description;
};

class Invite {
    public:
        static std::shared_ptr<Invite> Create(const InviteParams&);
        static std::shared_ptr<Invite> Decode(std::string_view);
        ~Invite();
        std::string Encode() const;
        std::string SessionId();
        std::string SessionUrl();
        std::vector<std::byte> EncryptJson(const nlohmann::json&);
        nlohmann::json DecryptJson(const std::vector<std::byte>&);

        InviteParams            params_ = {};

    private:
        Invite() = default;
        Invite(const InviteParams& params)
            : params_(params) {};

        std::vector<uint8_t>    secret_key_ = std::vector<uint8_t>(hydro_secretbox_KEYBYTES);
        std::uint64_t           timestamp_ = 0;
};

} // namespace vacon
