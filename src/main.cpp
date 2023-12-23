#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

using std::string;

int camera_sender(const string&);
int decoder_receiver(const string&);
int sink_receiver(const string&);

int main(int argc, char **argv) try {
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
		}
	} else {
		std::cerr
			<< "Error: Usage: "
			<< argv[0]
			<< " < camera_sender | decoder_receiver | sink_receiver >"
			<< std::endl;
		return -1;
	}
} catch (const std::exception &e) {
	std::cerr << "Error: " << e.what() << std::endl;
	return -1;
}
