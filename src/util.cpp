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

#include "util.hpp"

#include <cstdarg>
#include <format>

#if defined(__linux__)
# include <sys/prctl.h>
#endif

#if defined(VACON_USE_BACKWARD)
# include <backward.hpp>
#endif

#include <plog/Init.h>
#include <plog/Appenders/ColorConsoleAppender.h>
#include <plog/Formatters/TxtFormatter.h>

extern "C" {
#include <libavutil/log.h>
}

namespace vacon {
namespace util {

#if defined(VACON_USE_BACKWARD)
backward::SignalHandling gBackwardSignalHandling;
#endif

void FfmpegLogCallback([[maybe_unused]] void *ptr,
                       int level, const char *fmt, std::va_list vl_orig)
{
    // Apparently ffmpeg doesn't do this.
    if (level > av_log_get_level()) {
        return;
    }

    std::string msg;
    std::va_list vl_copy1;
    std::va_list vl_copy2;

    va_copy(vl_copy1, vl_orig);
    va_copy(vl_copy2, vl_orig);

    // Calculate the required size of the log buffer.
    const int len = std::vsnprintf(nullptr, 0, fmt, vl_copy1);
    if (len < 0) {
        LOG_DEBUG << "vsnprintf() failed";
        va_end(vl_copy1);
        va_end(vl_copy2);
        return;
    }

    // Actually format the log message into a buffer and log it.
    if (len > 0) {
        msg.resize(len);
        if (std::vsnprintf(msg.data(), len + 1, fmt, vl_copy2) > 0) {
            if (msg.back() == '\n') {
                msg.resize(msg.length() - 1);
            }
            auto severity = FfmpegLogLevelToPlogSeverity(level);
            LOG(severity) << msg;
        }
    }

    // Cleanup.
    va_end(vl_copy1);
    va_end(vl_copy2);
}

plog::Severity FfmpegLogLevelToPlogSeverity(const int av_log_level)
{
    auto severity = plog::verbose;

    switch (av_log_level) {
    case AV_LOG_QUIET:      severity = plog::none;      break;
    case AV_LOG_PANIC:      severity = plog::fatal;     break;
    case AV_LOG_FATAL:      severity = plog::fatal;     break;
    case AV_LOG_ERROR:      severity = plog::error;     break;
    case AV_LOG_WARNING:    severity = plog::warning;   break;
    case AV_LOG_INFO:       severity = plog::info;      break;
    case AV_LOG_VERBOSE:    severity = plog::debug;     break;
    case AV_LOG_DEBUG:      // Fallthrough.
    case AV_LOG_TRACE:      // Fallthrough.
    default:                severity = plog::verbose;   break;
    }

    return severity;
}

int PlogSeverityToFfmpegLogLevel(const plog::Severity plog_severity)
{
    int av_log_level = AV_LOG_TRACE;

    switch (plog_severity) {
    case plog::none:    av_log_level = AV_LOG_QUIET;    break;
    case plog::fatal:   av_log_level = AV_LOG_FATAL;    break;
    case plog::error:   av_log_level = AV_LOG_ERROR;    break;
    case plog::warning: av_log_level = AV_LOG_WARNING;  break;
    case plog::info:    av_log_level = AV_LOG_INFO;     break;
    case plog::debug:   av_log_level = AV_LOG_VERBOSE;  break;
    case plog::verbose: // Fallthrough.
    default:            av_log_level = AV_LOG_VERBOSE;  break;
    }

    return av_log_level;
}

void SetupLogging(const int verbosity)
{
    auto severity = plog::none;

    switch (verbosity) {
    case 0:     severity = plog::error;     break;
    case 1:     severity = plog::warning;   break;
    case 2:     severity = plog::info;      break;
    case 3:     severity = plog::debug;     break;
    case 4:     // Fallthrough.
    default:    severity = plog::verbose;   break;
    }

    static plog::ColorConsoleAppender<plog::TxtFormatter> plogConsoleAppender;
    plog::init(severity, &plogConsoleAppender);

    av_log_set_level(PlogSeverityToFfmpegLogLevel(severity));
    av_log_set_callback(FfmpegLogCallback);
}

bool SetupRealtimePriority()
{
	const int min_fifo_prio = sched_get_priority_min(SCHED_FIFO);
	if (min_fifo_prio == -1) {
        LOG_DEBUG << std::format("sched_get_priority_min() failed: {}", std::strerror(errno));
		return false;
	}

	const struct sched_param param = {
		.sched_priority = min_fifo_prio + 1,
	};

    LOG_VERBOSE << std::format("Attempting to set scheduling policy SCHED_FIFO, priority {}",
                                param.sched_priority);

	if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        LOG_DEBUG << std::format("sched_setscheduler() failed: {}", std::strerror(errno));
		return false;
	}

#if defined(HAVE_PTHREAD_SETATTR_DEFAULT_NP)
    int ret;
	static pthread_attr_t attr;
	if ((ret = pthread_attr_init(&attr))) {
        LOG_DEBUG << std::format("pthread_attr_init() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO))) {
        LOG_DEBUG << std::format("pthread_attr_setschedpolicy() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_attr_setschedparam(&attr, &param))) {
        LOG_DEBUG << std::format("pthread_attr_setschedparam() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED))) {
        LOG_DEBUG << std::format("pthread_attr_setinheritsched() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_setattr_default_np(&attr))) {
        LOG_DEBUG << std::format("pthread_setattr_default_np() failed: {}", ret);
		return false;
	}
#endif

	return true;
}

void SetThreadName(const char *name)
{
#if defined(__linux__)
    prctl(PR_SET_NAME, name);
#endif
}

// from v4l-utils
std::string FourCcToString(uint32_t val)
{
    std::string s;

    s += val & 0x7f;
    s += (val >> 8) & 0x7f;
    s += (val >> 16) & 0x7f;
    s += (val >> 24) & 0x7f;
    if (val & (1U << 31))
        s += "-BE";
    return s;
}

} // namespace util
} // namespace vacon
