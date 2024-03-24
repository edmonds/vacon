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
# define BACKWARD_HAS_DWARF 1
# define BACKWARD_HAS_UNWIND 1
# include <backward.hpp>
#endif

#include <plog/Init.h>
#include <plog/Appenders/ColorConsoleAppender.h>
#include <plog/Formatters/TxtFormatter.h>

namespace vacon {
namespace util {

#if defined(VACON_USE_BACKWARD)
backward::SignalHandling g_backward_signal_handling;
#endif

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
