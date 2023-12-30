#include "rtp_depacketizer.hpp"

#include <cassert>
#include <cstring>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <rtc/rtc.hpp>

#include "common.hpp"

namespace vacon {

std::unique_ptr<RtpDepacketizer> RtpDepacketizer::Create() {
    auto rd = std::make_unique<RtpDepacketizer>(RtpDepacketizer {});

    if (!rd->initFfmpeg()) {
        PLOG_FATAL << "initFfmpeg() failed";
        return nullptr;
    }

    return rd;
}

RtpDepacketizer::~RtpDepacketizer()
{
    PLOG_VERBOSE << fmt::format("Destructor called on {}", fmt::ptr(this));
    avformat_free_context(fctx);
    avio_context_free(&rtp_ioctx);
    avio_context_free(&sdp_ioctx);
}

bool RtpDepacketizer::initFfmpeg()
{
    int ret = 0;
    bool res = false;

    AVDictionary *sdp_options = nullptr;

    // Fake hardcoded SDP descriptor.
    // XXX: Generate this dynamically.
    std::string sdp_string(
        "c=IN IP4 127.0.0.1\n"
        "m=video 5000 RTP/AVP 96\n"
        "a=rtpmap:96 H265/90000\n"
    );

    // Allocate temporary fixed size buffer for readAvioPacketString().
    //
    // We cannot store this buffer on the stack. This has to be av_malloc()'d
    // and leaked by us, because apparently a side effect of calling
    // avformat_open_input() below is that ffmpeg's ID3v2 parser will try to
    // parse the session descriptor (?) and then try to free the buffer (??).
    //
    // This is, uh, pretty weird, because we tell ffmpeg explicitly what format
    // to use to parse this via the third parameter to avformat_open_input():
    // "If non-NULL, this parameter forces a specific input format. Otherwise
    // the format is autodetected."
    //
    // Anyway, here's ffmpeg unconditionally calling the ID3v2 parser
    // immediately before the actual input format's parser is called, when a
    // custom AVIOContext* is provided (i.e. when ->pb is set, as we do below):
    //
    // https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/demux.c#L312-L316
    //
    // Don't believe me? Here's the stack trace from Valgrind:
    //
    // Thread 14 RTC poll:
    // Invalid free() / delete / delete[] / realloc()
    //    at 0x48431EF: free (in /usr/libexec/valgrind/vgpreload_memcheck-amd64-linux.so)
    //    by 0x5EFFD9A: ffio_ensure_seekback (aviobuf.c:1099)
    //    by 0x5F68B0E: id3v2_read_internal (id3v2.c:1105)
    //    by 0x5F1FD2C: avformat_open_input (demux.c:313)
    //    by 0x2421E0: vacon::RtpDepacketizer::initFfmpeg() (rtp_depacketizer.cpp:98)
    //    by 0x244685: vacon::RtpDepacketizer::Create() (rtp_depacketizer.cpp:18)
    //    by 0x22D4B1: vacon::NetworkHandler::createPeerConnection(…) (network_handler.cpp:181)
    //    by 0x22E239: vacon::NetworkHandler::onWsMessage(…) (network_handler.cpp:94)
    //    by …
    //  Address 0x1d469ac0 is on thread 14's stack
    //  in frame #4, created by vacon::RtpDepacketizer::initFfmpeg() (rtp_depacketizer.cpp:35)
    //
    //std::array<std::byte, 1024> sdp_buf;
    const size_t sdp_buf_size = 1024;
    unsigned char *sdp_buf = (unsigned char *)av_malloc(sdp_buf_size);
    assert(sdp_buf);

    // Find AVInputFormat* for the SDP input format.
    const AVInputFormat *sdp_iformat = av_find_input_format("sdp");
    if (!sdp_iformat) {
        goto cleanup;
    }

    // Create format options dictionary for the SDP input format, set
    // RTSP_FLAG_CUSTOM_IO. Freed with av_dict_free().
    ret = av_dict_set(&sdp_options, "sdp_flags", "custom_io", 0);
    if (ret < 0) {
        goto cleanup;
    }

    // Allocate AVFormatContext* for the RTP stream.
    this->fctx = avformat_alloc_context();
    if (!this->fctx) {
        goto cleanup;
    }

    // Latency hacks.
    this->fctx->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

    // Create custom AVIOContext for reading the fake session descriptor from
    // an in-memory buffer.
    this->sdp_ioctx =
        avio_alloc_context(
            sdp_buf,                                            // buffer
            sdp_buf_size,                                       // buffer_size
            0,                                                  // write_flag
            static_cast<void*>(&sdp_string),                    // opaque
            readAvioPacketString,                               // read_packet
            nullptr,                                            // write_packet
            nullptr);                                           // seek
    if (!sdp_ioctx) {
        goto cleanup;
    }

    // Plug the custom AVIOContext for reading the fake session descriptor into
    // the AVFormatContext.
    this->fctx->pb = sdp_ioctx;

    // Open the RTP input stream. This will read the fake in-memory session
    // descriptor. Must be closed with avformat_close_input().
    ret = avformat_open_input(&this->fctx, NULL, sdp_iformat, &sdp_options);
    if (ret != 0) {
        goto cleanup;
    }

    // Now that the fake in-memory SDP has been completely read by
    // avformat_open_input(), swap out the AVFormatContext's I/O context
    // for a new AVIOContext that reads packets from the RTP stream.

    // Create custom AVIOContext for reading RTP packets from a callback.
    this->rtp_ioctx =
        avio_alloc_context(
            reinterpret_cast<unsigned char*>(this->buf.data()), // buffer
            this->buf.size(),                                   // buffer size
            1,                                                  // write_flag
            static_cast<void*>(&this->rtp_packet_queue),        // opaque
            readAvioPacketRTP,                                  // read_packet
            writeAvioPacketRTP,                                 // write_packet
            nullptr);                                           // seek
    if (!this->rtp_ioctx) {
        goto cleanup;
    }

    // Plug the custom AVIOContext for reading RTP packets into the
    // AVFormatContext.
    this->fctx->pb = this->rtp_ioctx;

    // Success.
    res = true;

cleanup:
    av_dict_free(&sdp_options);

    return res;
}

int RtpDepacketizer::readAvioPacketString(void *opaque, uint8_t *buf, int buf_size)
{
    std::string &s = *(static_cast<std::string*>(opaque));
    buf_size = std::min(buf_size, (int)s.length());
    const char *data = s.c_str();

    if (buf_size == 0) {
        return AVERROR_EOF;
    }

    memcpy(buf, data, (size_t)buf_size);
    s.erase(0, buf_size);

    return buf_size;
}

int RtpDepacketizer::readAvioPacketRTP(void *opaque, uint8_t *buf, int buf_size)
{
    moodycamel::BlockingReaderWriterQueue<rtc::binary> &rtp_packet_queue =
        *(static_cast<moodycamel::BlockingReaderWriterQueue<rtc::binary>*>(opaque));

    rtc::binary packet;
    static long count_rtp_packets = 0;
    static long count_rtp_bytes = 0;

    for (;;) {
        rtp_packet_queue.wait_dequeue(packet);
        PLOG_DEBUG << fmt::format("Dequeued RTP packet size {}", packet.size());
        if (gSignalUSR1) {
            gSignalUSR1 = 0;
            PLOG_DEBUG << "Dropping a packet due to signal!";
            continue;
        } else {
            break;
        }
    }

    assert(packet.size() > 0);
    assert((int)packet.size() <= buf_size);

    count_rtp_packets += 1;
    count_rtp_bytes += packet.size();
    if ((count_rtp_packets % 10000) == 0) {
        PLOG_INFO
            << fmt::format("Received {} packets, {} bytes from RTP peer, queue size {}, max queue capacity {}",
                           count_rtp_packets,
                           count_rtp_bytes,
                           rtp_packet_queue.size_approx(),
                           rtp_packet_queue.max_capacity());
    }

    memcpy(buf, packet.data(), packet.size());

    return packet.size();
}

int RtpDepacketizer::writeAvioPacketRTP(void *opaque __attribute__((unused)),
                                        /* const */ uint8_t *buf __attribute__((unused)),
                                        int buf_size)
{
    PLOG_DEBUG << fmt::format("ffmpeg wants to write {} bytes to RTP peer, ignoring", buf_size);
    return buf_size;
}

void RtpDepacketizer::submitRtpPacket(rtc::binary packet)
{
    PLOG_VERBOSE << fmt::format("Enqueuing RTP packet size {}", packet.size());
    this->rtp_packet_queue.emplace(packet);
}

} // namespace vacon