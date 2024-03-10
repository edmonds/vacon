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

#include "invite.hpp"

#include <chrono>
#include <format>
#include <string>

#include <hydrogen.h>
#include <nlohmann/json.hpp>
#include <plog/Log.h>

#include "base64.hpp"

namespace vacon {

static const char kHydrogenContext[hydro_hash_CONTEXTBYTES] = {
     'V','a','c','o','n','I','n','v'
};

std::shared_ptr<Invite> Invite::Create(const InviteParams& params)
{
    auto invite = Invite(params);
    hydro_secretbox_keygen(invite.secret_key_.data());
    return std::make_shared<Invite>(invite);
}

std::shared_ptr<Invite> Invite::Decode(std::string_view data)
{
    // Consistency check.
    if (data.size() < 6 || !data.starts_with("vacon:")) {
        return nullptr;
    }

    // Remove the "vacon:" URI scheme.
    data.remove_prefix(6);

    try {
        // Decode the Base64 encoded data.
        auto base64_decoded = base64::decode(data);

        // Decode the MessagePack encoded data.
        auto message = nlohmann::json::from_msgpack(base64_decoded);

        // Construct an Invite with values extracted from the MessagePack data.
        auto invite = Invite(InviteParams {
            .signaling_server = message["s"].get<std::string>(),
            .description = message["d"].get<std::string>(),
        });
        auto key = message["k"].get<std::vector<uint8_t>>();
        invite.secret_key_.assign(key.begin(), key.end());

        // Success.
        return std::make_shared<Invite>(invite);
    } catch (const std::exception &e) {
        LOG_ERROR << "Unable to decode invite URI: " << e.what();
    }

    return nullptr;
}

Invite::~Invite()
{
    hydro_memzero(secret_key_.data(), secret_key_.size());
}

std::string Invite::Encode() const
{
    // Encapsulate the Invite's values into a message.
    nlohmann::json message = {
        { "d", params_.description },
        { "s", params_.signaling_server },
        { "k", secret_key_ },
    };

    // Encode the values using MessagePack.
    auto data = nlohmann::json::to_msgpack(message);

    // Construct a URL using the "vacon:" scheme and append the MessagePack
    // encoded data as the URL path.
    auto url = std::string("vacon:");
    url.append(base64::encode(data));

    return url;
}

std::string Invite::SessionId()
{
    // Get the current time in seconds since the Unix epoch.
    auto now = std::chrono::system_clock::now();
    timestamp_ = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // Truncate the timestamp using a prime modulus. This avoids some clock
    // skew between the conference participants but still has a hard cutoff at
    // multiples of the modulus. Using a prime modulus rather than a round
    // number like 3600 will avoid having a hard cutoff on hour boundaries most
    // of the time.
    timestamp_ -= (timestamp_ % 7213);

    // Convert the timestamp to a string.
    auto ts = std::format("{}", timestamp_);

    // Hash the truncated timestamp using the Invite's secret key.
    auto hash = std::vector<uint8_t>(hydro_hash_BYTES);
    hydro_hash_hash(hash.data(), hash.size(),
                    ts.data(), ts.size(),
                    kHydrogenContext,
                    secret_key_.data());

    // Encode the hashed value.
    return base64::encode(hash);
}

std::string Invite::SessionUrl()
{
    return std::format("wss://{}/api/v1/offer-answer?{}", params_.signaling_server, SessionId());
}

} // namespace vacon
