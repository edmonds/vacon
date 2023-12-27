#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

#include <pthread.h>
#include <sched.h>

using std::string;

int camera_sender(const string&);
int decoder_receiver(const string&);
int sink_receiver(const string&);
int video_receiver(const string&);

volatile std::sig_atomic_t gSignalUSR1;

static void signalHandler(int signal) {
	if (signal == SIGUSR1) {
		gSignalUSR1 = 1;
	}
}

static bool setupRealtimePriority()
{
	const int min_fifo_prio = sched_get_priority_min(SCHED_FIFO);
	if (min_fifo_prio == -1) {
		std::cerr
			<< "sched_get_priority_min() failed: "
			<< std::strerror(errno)
			<< std::endl;
		return false;
	}

	const struct sched_param param = {
		.sched_priority = min_fifo_prio + 1,
	};

	if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
		std::cerr
			<< "pthread_setschedparam() failed: "
			<< std::strerror(errno)
			<< std::endl;
		return false;
	}

	static pthread_attr_t attr;
	if (pthread_attr_init(&attr)) {
		std::cerr << "pthread_attr_init() failed!" << std::endl;
		return false;
	}

	if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO)) {
		std::cerr << "pthread_attr_setschedpolicy() failed!" << std::endl;
		return false;
	}

	if (pthread_attr_setschedparam(&attr, &param)) {
		std::cerr << "pthread_attr_setschedparam() failed!" << std::endl;
		return false;
	}

	if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED)) {
		std::cerr << "pthread_attr_setinheritsched() failed!" << std::endl;
		return false;
	}

	if (pthread_setattr_default_np(&attr)) {
		std::cerr << "pthread_setattr_default_np() failed!" << std::endl;
		return false;
	}

	return true;
}

int main(int argc, char **argv) try
{
	setupRealtimePriority();

	std::signal(SIGUSR1, signalHandler);

	const char *ws_url_cstr = getenv("VACON_SIGNALING_URL");
	if (!ws_url_cstr) {
		std::cerr
			<< "Error: Environment variable VACON_SIGNALING_URL must be set."
			<< std::endl;
		return -1;
	}
	string ws_url(ws_url_cstr);

	if (argc == 2) {
		if (strcmp(argv[1], "camera_sender") == 0) {
			return camera_sender(ws_url);
		} else if (strcmp(argv[1], "decoder_receiver") == 0) {
			return decoder_receiver(ws_url);
		} else if (strcmp(argv[1], "sink_receiver") == 0) {
			return sink_receiver(ws_url);
		} else if (strcmp(argv[1], "video_receiver") == 0) {
			return video_receiver(ws_url);
		}
	} else {
		std::cerr
			<< "Error: Usage: "
			<< argv[0]
			<< " {camera_sender | decoder_receiver | sink_receiver | video_receiver}"
			<< std::endl;
		return -1;
	}
} catch (const std::exception &e) {
	std::cerr << "Error: " << e.what() << std::endl;
	return -1;
}
