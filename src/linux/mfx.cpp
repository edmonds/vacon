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

#include "linux/mfx.hpp"

#include <format>
#include <string>

#include <mfx.h>
#include <plog/Log.h>

namespace vacon {
namespace linux {

bool SetMfxLoaderConfigFilters(mfxLoader loader, mfxConfigFilters filters)
{
    for (auto& [name, value] : filters) {
        mfxConfig cfg = MFXCreateConfig(loader);
        if (!cfg) {
            LOG_ERROR << "MFXCreateConfig() failed";
            return false;
        }

        mfxVariant cfgVal = {};
        cfgVal.Type = MFX_VARIANT_TYPE_U32;
        cfgVal.Data.U32 = value;

        auto status = MFXSetConfigFilterProperty(cfg, (mfxU8*)name, cfgVal);
        if (status != MFX_ERR_NONE) {
            LOG_ERROR << std::format("MFXSetConfigFilterProperty({} = {:#010x}) failed: {}", name, value, (int)status);
            return false;
        } else {
            LOG_DEBUG << std::format("{} = {:#010x}", name, value);
        }
    }

    // Success.
    return true;
}

bool SetMfxLoaderConfigFiltersCombined(mfxLoader loader, mfxConfigFilters filters)
{
    mfxConfig cfg = MFXCreateConfig(loader);
    if (!cfg) {
        LOG_ERROR << "MFXCreateConfig() failed";
        return false;
    }

    for (auto& [name, value] : filters) {
        mfxVariant cfgVal = {};
        cfgVal.Type = MFX_VARIANT_TYPE_U32;
        cfgVal.Data.U32 = value;

        auto status = MFXSetConfigFilterProperty(cfg, (mfxU8*)name, cfgVal);
        if (status != MFX_ERR_NONE) {
            LOG_ERROR << std::format("MFXSetConfigFilterProperty({} = {:#010x}) failed: {}", name, value, (int)status);
            return false;
        } else {
            LOG_DEBUG << std::format("{} = {:#010x}", name, value);
        }
    }

    // Success.
    return true;
}

const char* MfxStatusStringConstant(mfxStatus status)
{
    switch (status) {
    case MFX_ERR_ABORTED:                   return "MFX_ERR_ABORTED";
    case MFX_ERR_DEVICE_FAILED:             return "MFX_ERR_DEVICE_FAILED";
    case MFX_ERR_DEVICE_LOST:               return "MFX_ERR_DEVICE_LOST";
    case MFX_ERR_GPU_HANG:                  return "MFX_ERR_GPU_HANG";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM:  return "MFX_ERR_INCOMPATIBLE_VIDEO_PARAM";
    case MFX_ERR_INVALID_HANDLE:            return "MFX_ERR_INVALID_HANDLE";
    case MFX_ERR_INVALID_VIDEO_PARAM:       return "MFX_ERR_INVALID_VIDEO_PARAM";
    case MFX_ERR_LOCK_MEMORY:               return "MFX_ERR_LOCK_MEMORY";
    case MFX_ERR_MEMORY_ALLOC:              return "MFX_ERR_MEMORY_ALLOC";
    case MFX_ERR_MORE_BITSTREAM:            return "MFX_ERR_MORE_BITSTREAM";
    case MFX_ERR_MORE_DATA:                 return "MFX_ERR_MORE_DATA";
    case MFX_ERR_MORE_DATA_SUBMIT_TASK:     return "MFX_ERR_MORE_DATA_SUBMIT_TASK";
#ifdef ONEVPL_EXPERIMENTAL
    case MFX_ERR_MORE_EXTBUFFER:            return "MFX_ERR_MORE_EXTBUFFER";
#endif
    case MFX_ERR_MORE_SURFACE:              return "MFX_ERR_MORE_SURFACE";
    case MFX_ERR_NONE:                      return "MFX_ERR_NONE";
    case MFX_ERR_NONE_PARTIAL_OUTPUT:       return "MFX_ERR_NONE_PARTIAL_OUTPUT";
    case MFX_ERR_NOT_ENOUGH_BUFFER:         return "MFX_ERR_NOT_ENOUGH_BUFFER";
    case MFX_ERR_NOT_FOUND:                 return "MFX_ERR_NOT_FOUND";
    case MFX_ERR_NOT_IMPLEMENTED:           return "MFX_ERR_NOT_IMPLEMENTED";
    case MFX_ERR_NOT_INITIALIZED:           return "MFX_ERR_NOT_INITIALIZED";
    case MFX_ERR_NULL_PTR:                  return "MFX_ERR_NULL_PTR";
    case MFX_ERR_REALLOC_SURFACE:           return "MFX_ERR_REALLOC_SURFACE";
    case MFX_ERR_RESOURCE_MAPPED:           return "MFX_ERR_RESOURCE_MAPPED";
    case MFX_ERR_UNDEFINED_BEHAVIOR:        return "MFX_ERR_UNDEFINED_BEHAVIOR";
    case MFX_ERR_UNKNOWN:                   return "MFX_ERR_UNKNOWN";
    case MFX_ERR_UNSUPPORTED:               return "MFX_ERR_UNSUPPORTED";
    case MFX_TASK_BUSY:                     return "MFX_TASK_BUSY";
    case MFX_TASK_WORKING:                  return "MFX_TASK_WORKING";
    case MFX_WRN_ALLOC_TIMEOUT_EXPIRED:     return "MFX_WRN_ALLOC_TIMEOUT_EXPIRED";
    case MFX_WRN_DEVICE_BUSY:               return "MFX_WRN_DEVICE_BUSY";
    case MFX_WRN_FILTER_SKIPPED:            return "MFX_WRN_FILTER_SKIPPED";
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM:  return "MFX_WRN_INCOMPATIBLE_VIDEO_PARAM";
    case MFX_WRN_IN_EXECUTION:              return "MFX_WRN_IN_EXECUTION";
    case MFX_WRN_OUT_OF_RANGE:              return "MFX_WRN_OUT_OF_RANGE";
    case MFX_WRN_PARTIAL_ACCELERATION:      return "MFX_WRN_PARTIAL_ACCELERATION";
    case MFX_WRN_VALUE_NOT_CHANGED:         return "MFX_WRN_VALUE_NOT_CHANGED";
    case MFX_WRN_VIDEO_PARAM_CHANGED:       return "MFX_WRN_VIDEO_PARAM_CHANGED";
    default:                                return "<UNKNOWN>";
    }
}

std::string MfxStatusStr(mfxStatus status)
{
    return std::format("{} ({})", MfxStatusStringConstant(status), static_cast<int>(status));
}

} // namespace linux
} // namespace vacon
