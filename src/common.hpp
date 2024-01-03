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

#pragma once

#include <cstdarg>
#include <csignal>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <plog/Log.h>

extern "C" {

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>

extern int plplay_play(AVFormatContext *);
extern void plplay_shutdown(void);

} // extern "C"

// Replacement for the ffmpeg av_err2str() macro which doesn't work in C++.
#ifdef av_err2str
#undef av_err2str
av_always_inline std::string av_err2string(int errnum) {
	char str[AV_ERROR_MAX_STRING_SIZE];
	return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#define av_err2str(err) av_err2string(err).c_str()
#endif

namespace vacon {

template <class T> std::weak_ptr<T> make_weak_ptr(std::shared_ptr<T> ptr) { return ptr; }

extern volatile std::sig_atomic_t gSignalUSR1;

void ffmpegLogCallback(void *ptr, int level, const char *fmt, std::va_list vl_orig);
plog::Severity ffmpegLogLevelToPlogSeverity(const int av_log_level);
int plogSeverityToFfmpegLogLevel(const plog::Severity plog_severity);
void setupLogging(const int verbosity);
bool setupRealtimePriority();
void setThreadName(const char *name);

} // namespace vacon
