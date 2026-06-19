#pragma once

#include <cstddef>
#include <memory>

#include <boost/asio/buffer.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

namespace testcontainers {
class DockerHost;
}

namespace testcontainers::docker {

/// A bidirectional byte stream to the Docker daemon, abstracting over the
/// concrete transport (unix socket / Windows named pipe / TCP / TLS).
class ITransport {
public:
    virtual ~ITransport() = default;
    virtual std::size_t read_some(void* data, std::size_t size,
                                  boost::system::error_code& ec) = 0;
    virtual std::size_t write_some(const void* data, std::size_t size,
                                   boost::system::error_code& ec) = 0;
    /// Half-close the send side so the peer sees EOF on its read while we keep
    /// reading the response (used by exec-stdin: after writing the input bytes we
    /// signal end-of-input so a reader like `cat`/`wc` terminates). Best-effort:
    /// SSL and Windows named pipes have no clean half-close, so those override it
    /// as a no-op (see the transport implementations).
    virtual void shutdown_send() = 0;
    virtual void close() = 0;
};

/// Adapts an ITransport into a Beast SyncReadStream / SyncWriteStream so the
/// Beast HTTP serializer/parser can run over any transport. Beast requires the
/// templated `read_some`/`write_some` member shapes provided below; they
/// forward to the (type-erased) virtual ITransport.
class TransportStream {
public:
    explicit TransportStream(ITransport& transport) noexcept : transport_(&transport) {}

    template <class MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers, boost::system::error_code& ec) {
        for (auto it = boost::asio::buffer_sequence_begin(buffers);
             it != boost::asio::buffer_sequence_end(buffers); ++it) {
            const boost::asio::mutable_buffer b(*it);
            if (b.size() != 0) {
                return transport_->read_some(b.data(), b.size(), ec);
            }
        }
        ec.clear();
        return 0;
    }

    template <class MutableBufferSequence>
    std::size_t read_some(const MutableBufferSequence& buffers) {
        boost::system::error_code ec;
        const std::size_t n = read_some(buffers, ec);
        if (ec) {
            throw boost::system::system_error(ec);
        }
        return n;
    }

    template <class ConstBufferSequence>
    std::size_t write_some(const ConstBufferSequence& buffers, boost::system::error_code& ec) {
        for (auto it = boost::asio::buffer_sequence_begin(buffers);
             it != boost::asio::buffer_sequence_end(buffers); ++it) {
            const boost::asio::const_buffer b(*it);
            if (b.size() != 0) {
                return transport_->write_some(b.data(), b.size(), ec);
            }
        }
        ec.clear();
        return 0;
    }

    template <class ConstBufferSequence>
    std::size_t write_some(const ConstBufferSequence& buffers) {
        boost::system::error_code ec;
        const std::size_t n = write_some(buffers, ec);
        if (ec) {
            throw boost::system::system_error(ec);
        }
        return n;
    }

private:
    ITransport* transport_;
};

/// Open a fresh transport connection to the given Docker host.
std::unique_ptr<ITransport> connect(const DockerHost& host);

} // namespace testcontainers::docker
