#include "common.hpp"

#include <cstdarg>

#if defined(__linux__)
# include <sys/prctl.h>
#endif

#include <plog/Init.h>
#include <plog/Appenders/ColorConsoleAppender.h>
#include <plog/Formatters/TxtFormatter.h>

namespace vacon {

void ffmpegLogCallback(void *ptr __attribute__((unused)),
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
        PLOG_DEBUG << "vsnprintf() failed";
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
            auto severity = ffmpegLogLevelToPlogSeverity(level);
            PLOG(severity) << msg;
        }
    }

    // Cleanup.
    va_end(vl_copy1);
    va_end(vl_copy2);
}

plog::Severity ffmpegLogLevelToPlogSeverity(const int av_log_level)
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

int plogSeverityToFfmpegLogLevel(const plog::Severity plog_severity)
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

void setupLogging(const int verbosity)
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

    av_log_set_level(plogSeverityToFfmpegLogLevel(severity));
    av_log_set_callback(ffmpegLogCallback);
}

bool setupRealtimePriority()
{
	const int min_fifo_prio = sched_get_priority_min(SCHED_FIFO);
	if (min_fifo_prio == -1) {
        PLOG_DEBUG << fmt::format("sched_get_priority_min() failed: {}", std::strerror(errno));
		return false;
	}

	const struct sched_param param = {
		.sched_priority = min_fifo_prio + 1,
	};

    PLOG_DEBUG << fmt::format("Attempting to set scheduling policy SCHED_FIFO, priority {}",
                         param.sched_priority);

	if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        PLOG_DEBUG << fmt::format("sched_setscheduler() failed: {}", std::strerror(errno));
		return false;
	}

#if defined(HAVE_PTHREAD_SETATTR_DEFAULT_NP)
    int ret;
	static pthread_attr_t attr;
	if ((ret = pthread_attr_init(&attr))) {
        PLOG_DEBUG << fmt::format("pthread_attr_init() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO))) {
        PLOG_DEBUG << fmt::format("pthread_attr_setschedpolicy() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_attr_setschedparam(&attr, &param))) {
        PLOG_DEBUG << fmt::format("pthread_attr_setschedparam() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED))) {
        PLOG_DEBUG << fmt::format("pthread_attr_setinheritsched() failed: {}", ret);
		return false;
	}

	if ((ret = pthread_setattr_default_np(&attr))) {
        PLOG_DEBUG << fmt::format("pthread_setattr_default_np() failed: {}", ret);
		return false;
	}
#endif

	return true;
}

void setThreadName(const char *name)
{
#if defined(__linux__)
    prctl(PR_SET_NAME, name);
#endif
}

} // namespace vacon
