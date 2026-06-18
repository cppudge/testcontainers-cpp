#include "docker/LogDemux.hpp"

namespace testcontainers::docker {

namespace {

LogStreamKind stream_from_byte(unsigned char type) {
    switch (type) {
    case 0:
        return LogStreamKind::StdIn;
    case 2:
        return LogStreamKind::StdErr;
    case 1:
    default:
        // Unknown stream bytes are treated as stdout — Docker only emits 0/1/2.
        return LogStreamKind::StdOut;
    }
}

} // namespace

std::vector<LogFrame> LogDemuxer::feed(std::string_view bytes) {
    std::vector<LogFrame> frames;
    pending_.append(bytes.data(), bytes.size());

    std::size_t pos = 0;
    while (true) {
        if (!have_header_) {
            if (pending_.size() - pos < kHeaderSize) {
                break; // header not fully buffered yet
            }
            const auto* h = reinterpret_cast<const unsigned char*>(pending_.data() + pos);
            stream_ = stream_from_byte(h[0]);
            payload_len_ = (static_cast<std::uint32_t>(h[4]) << 24) |
                           (static_cast<std::uint32_t>(h[5]) << 16) |
                           (static_cast<std::uint32_t>(h[6]) << 8) |
                           static_cast<std::uint32_t>(h[7]);
            have_header_ = true;
            pos += kHeaderSize;
        }

        if (pending_.size() - pos < payload_len_) {
            break; // payload not fully buffered yet
        }

        LogFrame frame;
        frame.stream = stream_;
        frame.data.assign(pending_, pos, payload_len_);
        frames.push_back(std::move(frame));

        pos += payload_len_;
        have_header_ = false;
        payload_len_ = 0;
    }

    // Drop the bytes we consumed; keep the unparsed remainder for the next feed.
    if (pos > 0) {
        pending_.erase(0, pos);
    }
    return frames;
}

DemuxedLogs demux_all(std::string_view body) {
    LogDemuxer demuxer;
    DemuxedLogs out;
    for (const auto& frame : demuxer.feed(body)) {
        switch (frame.stream) {
        case LogStreamKind::StdOut:
            out.stdout_data += frame.data;
            break;
        case LogStreamKind::StdErr:
            out.stderr_data += frame.data;
            break;
        case LogStreamKind::StdIn:
            break; // never present in log output
        }
    }
    return out;
}

} // namespace testcontainers::docker
