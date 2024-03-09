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

#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <fontconfig/fontconfig.h>
#include <plog/Log.h>

namespace vacon {
namespace linux {

std::optional<std::string> GetTrueTypeFileNameByPattern(std::string_view name)
{
    std::optional<std::string> result = std::nullopt;

    FcConfig*       config = nullptr;
    FcPattern*      pattern = nullptr;
    FcObjectSet*    objectset = nullptr;
    FcFontSet*      fontset = nullptr;
    FcResult        fcresult = {};

    auto name_str = std::string(name);
    auto name_cstr = reinterpret_cast<const FcChar8 *>(name_str.c_str());

    config = FcInitLoadConfigAndFonts();
    if (!config) {
        LOG_ERROR << "FcInitLoadConfigAndFonts() failed";
        goto err;
    }

    pattern = FcNameParse(name_cstr);
    if (!pattern) {
        LOG_ERROR << "FcNameParse() failed";
        goto err;
    }

    if (FcConfigSubstitute(config, pattern, FcMatchPattern) != FcTrue) {
        LOG_ERROR << "FcConfigSubstitute() failed";
        goto err;
    }

    FcDefaultSubstitute(pattern);

    objectset = FcObjectSetBuild(FC_FILE, FC_FONTFORMAT, nullptr);
    if (!objectset) {
        LOG_ERROR << "FcObjectSetBuild() failed";
        goto err;
    }

    fontset = FcFontSort(config, pattern, FcTrue, 0, &fcresult);
    if (fcresult != FcResultMatch) {
        LOG_ERROR << "FcFontSort() failed";
        goto err;
    }

    for (int i = 0; i < fontset->nfont; ++i) {
        auto font = fontset->fonts[i];
        if (!font) {
            LOG_DEBUG << std::format("font {} in fontset is null", i);
            continue;
        }

        FcChar8* fc_filename = nullptr;
        FcPatternGetString(font, FC_FILE, 0, &fc_filename);
        if (!fc_filename) {
            LOG_DEBUG << std::format("font {} in fontset has no filename", i);
            continue;
        }

        FcChar8* fc_fontformat = nullptr;
        FcPatternGetString(font, FC_FONTFORMAT, 0, &fc_fontformat);
        if (!fc_fontformat) {
            LOG_DEBUG << std::format("font {} in fontset has no fontformat", i);
            continue;
        }

        std::string filename(reinterpret_cast<char*>(fc_filename));
        std::string fontformat(reinterpret_cast<char*>(fc_fontformat));

        LOG_VERBOSE << std::format("Found {} fonts matching '{}', using '{}'",
                                   fontset->nfont, name, filename);
        result = filename;
        break;
    }

    if (!result.has_value()) {
        LOG_WARNING << std::format("No font found matching '{}'", name);
    }

err:
    if (fontset) {
        FcFontSetSortDestroy(fontset);
    }

    if (objectset) {
        FcObjectSetDestroy(objectset);
    }

    if (pattern) {
        FcPatternDestroy(pattern);
    }

    if (config) {
        FcConfigDestroy(config);
    }

    FcFini();

    return result;
}

} // namespace linux
} // namespace vacon
