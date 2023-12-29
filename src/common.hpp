#pragma once

#include <cstdarg>
#include <csignal>
#include <string>

#include <fmt/format.h>
#include <plog/Log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

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

extern volatile std::sig_atomic_t gSignalUSR1;

void ffmpegLogCallback(void *ptr, int level, const char *fmt, std::va_list vl_orig);
plog::Severity ffmpegLogLevelToPlogSeverity(const int av_log_level);
int plogSeverityToFfmpegLogLevel(const plog::Severity plog_severity);
void setupLogging(const int verbosity);
bool setupRealtimePriority();

} // namespace vacon
