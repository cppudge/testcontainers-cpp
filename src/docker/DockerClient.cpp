#include "testcontainers/docker/DockerClient.hpp"

#include "Strings.hpp"
#include "docker/ApiMapping.hpp"
#include "docker/Auth.hpp"
#include "docker/LogDemux.hpp"
#include "docker/StreamRead.hpp"
#include "docker/Tar.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/version.hpp"

#include <boost/asio/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http.hpp>
#include <boost/optional.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace testcontainers {

namespace {

namespace http = boost::beast::http;

/// Percent-encode a query-parameter value (leaves unreserved chars and '/').
std::string url_encode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

/// A request carrying the headers every Docker call shares (Host, User-Agent)
/// with keep-alive off (one connection per request for now).
template <class Body>
http::request<Body> make_request(http::verb method, const std::string& target,
                                 const DockerHost& host) {
    http::request<Body> req{method, target, /*HTTP*/ 11};
    req.set(http::field::host, host.http_host());
    req.set(http::field::user_agent, "testcontainers-cpp/" + version());
    req.keep_alive(false);
    return req;
}

/// Best-effort bounded drain of an ERROR response's body so the daemon's JSON
/// `{"message": ...}` can be appended to the thrown error. Returns "" on any
/// read failure (diagnostics only — never throws).
inline std::string read_error_body(docker::TransportStream& stream,
                                   boost::beast::flat_buffer& buffer,
                                   http::response_parser<http::buffer_body>& parser) {
    std::string out;
    std::array<char, 2048> buf{};
    boost::system::error_code ec;
    while (!parser.is_done() && out.size() < 8192) {
        parser.get().body().data = buf.data();
        parser.get().body().size = buf.size();
        http::read_some(stream, buffer, parser, ec);
        if (ec == http::error::need_buffer) {
            ec = {};
        }
        if (ec) {
            break;
        }
        out.append(buf.data(), buf.size() - parser.get().body().size);
    }
    return out;
}

/// Throw the typed error for a non-OK daemon reply: NotFoundError on 404,
/// DockerError carrying the HTTP status otherwise. The message is
/// "<context> failed: HTTP <status> <body>". `resource_id` names the
/// container / image / network / volume / exec instance the call was about
/// ("" when the call has no single subject).
[[noreturn]] void throw_status_error(const std::string& context, int status,
                                     const std::string& body, const std::string& resource_id = {}) {
    const std::string msg =
        context + " failed: HTTP " + std::to_string(status) + (body.empty() ? "" : " " + body);
    if (status == 404) {
        throw NotFoundError(msg, resource_id);
    }
    throw DockerError(msg, status, resource_id);
}

[[noreturn]] void throw_status_error(const std::string& context, const Response& res,
                                     const std::string& resource_id = {}) {
    throw_status_error(context, res.status_code, res.body, resource_id);
}

/// Throw for a failed body write of a streamed (chunked) upload. The daemon
/// may have REJECTED the upload and closed its receive side while blocks were
/// still going out — the write error (broken pipe / reset) is then just the
/// symptom — so first try to read its response off the connection: an error
/// status is the better diagnosis and is thrown as the typed status error.
/// Only when no failing response can be read does the transport error (with
/// a deadline expiry mapped to TransportTimeoutError) surface. Closes
/// `transport` either way. Best-effort by nature: a TCP reset can destroy
/// the buffered response, and a broken Windows named pipe delivers no
/// buffered bytes at all — the transport error is the fallback diagnosis.
[[noreturn]] void throw_upload_error(docker::ITransport& transport, docker::TransportStream& stream,
                                     const std::string& context, const std::string& resource_id,
                                     const boost::system::error_code& write_ec) {
    // A deadline expiry means a WEDGED peer, not a rejecting one: no early
    // response is coming, and the speculative read would burn a second full
    // io budget before failing the same way.
    if (write_ec == boost::asio::error::timed_out) {
        transport.close();
        docker::throw_transport_error(
            context + " failed to send request body: " + write_ec.message(), write_ec);
    }
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(8192); // diagnostics only — the error JSON is small
    boost::system::error_code ec;
    http::read(stream, buffer, parser, ec);
    const bool have_status = parser.is_header_done();
    const int status = have_status ? static_cast<int>(parser.get().result_int()) : 0;
    const std::string body = have_status ? parser.get().body() : std::string{};
    transport.close();
    if (have_status && (status < 200 || status > 299)) {
        throw_status_error(context, status, body, resource_id);
    }
    docker::throw_transport_error(context + " failed to send request body: " + write_ec.message(),
                                  write_ec);
}

/// Send `req` with `Transfer-Encoding: chunked`, writing the header and then
/// streaming the body blocks `producer` pushes into the sink (each block goes
/// out as one HTTP chunk; returning from `producer` sends the terminating
/// chunk). On a write failure the daemon's early error response wins over the
/// raw transport error (see throw_upload_error); a producer exception closes
/// the transport and propagates unchanged. On success the connection is left
/// ready for the response read.
void write_chunked_request(docker::ITransport& transport, docker::TransportStream& stream,
                           http::request<http::buffer_body>& req,
                           const docker::BodyProducer& producer, const std::string& context,
                           const std::string& resource_id) {
    req.chunked(true);
    http::request_serializer<http::buffer_body> serializer{req};

    boost::system::error_code ec;
    http::write_header(stream, serializer, ec);
    if (ec) {
        transport.close();
        docker::throw_transport_error(context + " failed to send request: " + ec.message(), ec);
    }

    const docker::ByteSink sink = [&](const char* data, std::size_t size) {
        if (size == 0) {
            return; // a zero-length chunk would read as the body terminator
        }
        req.body().data = const_cast<char*>(data); // Beast only reads from it
        req.body().size = size;
        req.body().more = true;
        http::write(stream, serializer, ec);
        if (ec == http::error::need_buffer) {
            ec = {}; // the serializer consumed the block and wants the next
        }
        if (ec) {
            throw_upload_error(transport, stream, context, resource_id, ec);
        }
    };
    try {
        producer(sink);
    } catch (...) {
        // The producer failed (a host file vanished, the wire died): the
        // request body is unfinished, so the connection can carry nothing
        // else. close() is idempotent — a sink abort already closed it.
        transport.close();
        throw;
    }

    // End of body: the serializer emits the terminating chunk.
    req.body().data = nullptr;
    req.body().size = 0;
    req.body().more = false;
    http::write(stream, serializer, ec);
    if (ec) {
        throw_upload_error(transport, stream, context, resource_id, ec);
    }
}

/// Read the response header off a hijacked/streaming connection and require
/// HTTP 200 (or, with `accept_upgraded`, the 101 a connection-upgrade request
/// is answered with): on a read error, an immediate EOF, or a failing status
/// this closes `transport` and throws the typed error prefixed with `context`
/// (a deadline expiry becomes TransportTimeoutError, a bad status goes through
/// throw_status_error with the daemon's drained error body appended).
void read_ok_header(docker::ITransport& transport, docker::TransportStream& stream,
                    boost::beast::flat_buffer& buffer,
                    http::response_parser<http::buffer_body>& parser, const std::string& context,
                    const std::string& resource_id = {}, bool accept_upgraded = false) {
    boost::system::error_code ec;
    http::read_header(stream, buffer, parser, ec);
    if (ec == http::error::end_of_stream && !parser.is_header_done()) {
        // Without this check an immediate EOF would read the status off a
        // never-populated parser, masquerading as an empty 200 response.
        transport.close();
        throw DockerError(context + " failed: connection closed before a response header",
                          std::nullopt, resource_id);
    }
    if (ec && ec != http::error::end_of_stream) {
        transport.close();
        docker::throw_transport_error(context + " failed to read response: " + ec.message(), ec);
    }
    const int status = static_cast<int>(parser.get().result_int());
    if (status != 200 && !(accept_upgraded && status == 101)) {
        const std::string detail = read_error_body(stream, buffer, parser);
        transport.close();
        throw_status_error(context, status, detail, resource_id);
    }
}

} // namespace

std::string Response::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (boost::beast::iequals(key, name)) {
            return value;
        }
    }
    return {};
}

DockerClient::DockerClient(DockerHost host) : host_(std::move(host)) {}

DockerClient DockerClient::from_environment() { return DockerClient(DockerHost::resolve()); }

const std::string& DockerClient::api_prefix() {
    if (api_prefix_) {
        return *api_prefix_;
    }
    // One unversioned round-trip; the daemon's version middleware stamps its
    // newest supported version on EVERY reply (errors included), so the
    // header is read regardless of the ping's status. Only a reply without a
    // parsable Api-Version is cached as "no pin": unversioned paths keep
    // working against the daemon's default version. A TRANSPORT failure
    // propagates uncached — the call that triggered the negotiation would
    // have failed the same way, and a later retry gets a fresh chance to
    // negotiate.
    const Response res = request("GET", "/_ping");
    const std::string negotiated = docker::negotiate_api_version(res.header("Api-Version"));
    return api_prefix_.emplace(negotiated.empty() ? std::string{} : "/v" + negotiated);
}

std::string DockerClient::versioned(std::string_view target) {
    return api_prefix() + std::string(target);
}

Response DockerClient::request(std::string_view method, std::string_view target,
                               std::string_view body,
                               const std::vector<std::pair<std::string, std::string>>& headers) {
    return request_with_io_timeout(method, target, body, headers, timeouts_.io);
}

Response DockerClient::request_with_io_timeout(
    std::string_view method, std::string_view target, std::string_view body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::optional<std::chrono::milliseconds> io_timeout) {
    docker::TransportTimeouts timeouts = timeouts_;
    timeouts.io = io_timeout;

    // Session reuse applies to idempotent GETs only: a stale-connection retry
    // (below) can then never replay a side effect, and non-GET requests behave
    // exactly as without a session (fresh connection, close after). HEAD is
    // deliberately excluded: the response parser would wait for the body a
    // HEAD reply announces but never carries.
    const bool reuse = session_enabled_ && method == "GET";

    // One request/response exchange on `transport`. Sets `conn_reusable` to
    // whether the connection may serve another request afterwards (the whole
    // response arrived and neither side asked to close).
    const auto perform = [&](docker::ITransport& transport, bool keep_alive,
                             bool& conn_reusable) -> Response {
        conn_reusable = false;
        docker::TransportStream stream{transport};

        auto req = make_request<http::string_body>(http::string_to_verb(std::string(method)),
                                                   std::string(target), host_);
        req.keep_alive(keep_alive);
        for (const auto& [key, value] : headers) {
            req.set(key, value);
        }
        if (!body.empty()) {
            req.body().assign(body);
        }
        req.prepare_payload();

        boost::system::error_code ec;
        http::write(stream, req, ec);
        if (ec) {
            docker::throw_transport_error("Failed to send request to Docker (" +
                                              std::string(method) + " " + std::string(target) +
                                              "): " + ec.message(),
                                          ec);
        }

        boost::beast::flat_buffer buffer;
        // Beast's default 1 MiB body limit is replaced by the client's own
        // cap: none by default (Docker archive and log bodies comfortably
        // exceed 1 MiB and we read the whole body), or the caller's
        // set_max_response_body ceiling, which turns a runaway reply into a
        // typed error below instead of unbounded allocation.
        http::response_parser<http::string_body> parser;
        if (max_response_body_) {
            parser.body_limit(*max_response_body_);
        } else {
            parser.body_limit(boost::none);
        }
        if (method == "HEAD") {
            // A HEAD reply advertises the body it would have sent (e.g. the
            // archive size on /containers/{id}/archive) but never carries
            // it: without skip the parser would wait for those bytes forever.
            parser.skip(true);
        }
        http::read(stream, buffer, parser, ec);
        if (ec == http::error::end_of_stream && !parser.is_done()) {
            // EOF before a complete response. Without this check a stale
            // kept-alive connection (peer closed it while idle) would read a
            // never-populated parser and masquerade as an empty success.
            throw DockerError("Failed to read response from Docker (" + std::string(method) + " " +
                              std::string(target) +
                              "): connection closed before the response completed");
        }
        if (ec == http::error::body_limit) {
            throw DockerError("Failed to read response from Docker (" + std::string(method) + " " +
                              std::string(target) +
                              "): the response body exceeds the configured "
                              "max_response_body cap of " +
                              std::to_string(max_response_body_.value_or(0)) + " bytes");
        }
        if (ec && ec != http::error::end_of_stream) {
            docker::throw_transport_error("Failed to read response from Docker (" +
                                              std::string(method) + " " + std::string(target) +
                                              "): " + ec.message(),
                                          ec);
        }
        auto& res = parser.get();
        // need_eof() marks an EOF-delimited body: the peer ends the message by
        // closing, so the connection is not reusable even though keep_alive()
        // (a header-only check) may still say true. (Beast reports a completed
        // EOF-delimited read as SUCCESS — the end_of_stream branch above only
        // fires when zero response bytes arrived.)
        conn_reusable = keep_alive && res.keep_alive() && !res.need_eof();

        Response out;
        out.status_code = static_cast<int>(res.result_int());
        out.reason = std::string(res.reason());
        out.body = std::move(res.body());
        for (const auto& field : res) {
            out.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));
        }
        return out;
    };

    // Fast path: reuse the session's kept-alive connection. The daemon (or an
    // intermediary — Docker Desktop's proxy, NAT, podman's ~5s idle close) may
    // have closed it while idle, which surfaces as a failed write or an EOF/
    // reset on read; since only idempotent requests get here, retry ONCE on a
    // fresh connection. A deadline expiry is NOT staleness — retrying would
    // silently double the io budget — so it propagates.
    if (reuse && session_transport_) {
        auto transport = session_transport_;
        transport->set_io_timeout(timeouts.io);
        try {
            bool conn_reusable = false;
            Response out = perform(*transport, /*keep_alive*/ true, conn_reusable);
            if (!conn_reusable) {
                session_transport_.reset();
            }
            return out;
        } catch (const TransportTimeoutError&) {
            session_transport_.reset();
            throw;
        } catch (const DockerError&) {
            session_transport_.reset(); // stale — fall through to a fresh connection
        }
    }

    std::shared_ptr<docker::ITransport> transport = docker::connect(host_, timeouts);
    bool conn_reusable = false;
    Response out = perform(*transport, /*keep_alive*/ reuse, conn_reusable);
    if (reuse && conn_reusable) {
        session_transport_ = std::move(transport);
    } else {
        transport->close();
    }
    return out;
}

void DockerClient::end_session() noexcept {
    session_enabled_ = false;
    if (session_transport_) {
        try {
            session_transport_->close();
        } catch (...) {
            ; // teardown is best-effort; the destructor drops the handle anyway
        }
        session_transport_.reset();
    }
}

bool DockerClient::ping() { return request("GET", "/_ping").ok(); }

std::string DockerClient::server_os() {
    // The engine mode (Linux vs Windows containers) is fixed for the life of a
    // DAEMON, not the process — a process may talk to several daemons (e.g. both
    // Docker Desktop engines, or a local plus a remote one) — so the answer is
    // cached per endpoint. Guarded by a mutex because a process may share the
    // daemon across threads.
    static std::mutex cache_mutex;
    static std::map<std::string, std::string> cached; // endpoint URL -> Os
    const std::string endpoint = host_.to_string();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        const auto it = cached.find(endpoint);
        if (it != cached.end()) {
            return it->second;
        }
    }

    const Response res = request("GET", versioned("/version"));
    if (!res.ok()) {
        throw_status_error("server_os: GET /version", res);
    }
    std::string os = docker::parse_server_os(res.body);

    std::lock_guard<std::mutex> lock(cache_mutex);
    cached.emplace(endpoint, os);
    return os;
}

bool DockerClient::is_windows_engine() {
    // Case-insensitive "contains windows".
    return detail::to_lower(server_os()).find("windows") != std::string::npos;
}

void DockerClient::pull_image(const std::string& image, const std::optional<RegistryAuth>& auth) {
    const auto [name, tag] = docker::split_image(image);
    const std::string target =
        versioned("/images/create?fromImage=" + url_encode(name) + "&tag=" + url_encode(tag));

    // Use the explicit credentials if given, else auto-resolve from the Docker
    // config for this image's registry. No credentials -> a plain public pull.
    const std::optional<RegistryAuth> cred = auth ? auth : docker::resolve_auth_for_image(image);
    std::vector<std::pair<std::string, std::string>> headers;
    if (cred) {
        headers.emplace_back("X-Registry-Auth", docker::encode_x_registry_auth(*cred));
    }

    // A registry hiccup relayed by the daemon (its auth-token endpoint
    // answering 5xx, a blip mid-handshake) surfaces as a 5xx response header —
    // /images/create is idempotent, so those get a bounded retry. Everything
    // else fails on the first try: 4xx are permanent, and an error embedded in
    // the 200 progress stream is how most daemons report a nonexistent image
    // (some relay it as a header 500 instead — the retry then costs a few
    // extra seconds before the same failure).
    std::chrono::milliseconds delay = pull_retry_.first_delay;
    for (int attempt = 1;; ++attempt) {
        const Response res = request("POST", target, /*body*/ {}, headers);
        if (res.status_code == 200) {
            // Docker streams progress as newline-delimited JSON and returns 200
            // even on failure, embedding the error in the stream.
            docker::throw_if_pull_error(res.body, image);
            return;
        }
        if (attempt >= pull_retry_.attempts || res.status_code < 500 || res.status_code > 599) {
            throw_status_error("pull_image('" + image + "')", res, image);
        }
        std::this_thread::sleep_for(delay);
        delay *= 2;
    }
}

bool DockerClient::image_exists(const std::string& reference) {
    // The reference goes into the path verbatim: image references cannot contain
    // characters that break a path, and the daemon's route accepts ':' and '/'.
    const Response res = request("GET", versioned("/images/" + reference + "/json"));
    if (res.status_code == 200) {
        return true;
    }
    if (res.status_code == 404) {
        return false;
    }
    throw_status_error("image_exists('" + reference + "')", res, reference);
}

std::string DockerClient::inspect_image_raw(const std::string& reference) {
    // The reference goes into the path verbatim, exactly like image_exists.
    const Response res = request("GET", versioned("/images/" + reference + "/json"));
    if (res.status_code != 200) {
        // 404 ("no such image") becomes NotFoundError via throw_status_error.
        throw_status_error("inspect_image('" + reference + "')", res, reference);
    }
    return res.body;
}

ImageInspect DockerClient::inspect_image(const std::string& reference) {
    return docker::parse_image_inspect(inspect_image_raw(reference));
}

void DockerClient::build_image(const std::string& context_tar, const docker::BuildOptions& options,
                               const docker::BuildLogConsumer& consumer) {
    // One chunk carrying the whole (already in-memory) context; the wire
    // format is identical to the streaming overload's.
    build_image([&context_tar](
                    const docker::ByteSink& sink) { sink(context_tar.data(), context_tar.size()); },
                options, consumer);
}

void DockerClient::build_image(const docker::BodyProducer& context,
                               const docker::BuildOptions& options,
                               const docker::BuildLogConsumer& consumer) {
    const std::string target =
        versioned("/build" + docker::build_build_query(
                                 options, [](const std::string& v) { return url_encode(v); }));
    const std::string ctx = "build_image('" + options.tag + "')";

    // Stream both directions: the context goes out in chunks as it is
    // produced, and the response is decoded as the daemon emits it, so a
    // consumer sees steps live and a failure carries the output that
    // preceded it (mirrors follow_logs).
    auto transport = docker::connect(host_, timeouts_);
    docker::TransportStream stream{*transport};

    auto req = make_request<http::buffer_body>(http::verb::post, target, host_);
    req.set(http::field::content_type, "application/x-tar");
    write_chunked_request(*transport, stream, req, context, ctx, options.tag);

    boost::system::error_code ec;
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);

    read_ok_header(*transport, stream, buffer, parser, ctx, options.tag);

    // The legacy /build endpoint streams output only when a step produces it —
    // a silent RUN step can legitimately idle for minutes — so widen the io
    // deadline instead of misreading a quiet build as a wedged daemon.
    if (timeouts_.io) {
        transport->set_io_timeout(
            std::max(*timeouts_.io, std::chrono::milliseconds(std::chrono::minutes(10))));
    }

    docker::BuildStreamScanner scanner(options.tag, consumer);
    try {
        std::array<char, 8192> buf{};
        while (!parser.is_done()) {
            parser.get().body().data = buf.data();
            parser.get().body().size = buf.size();

            http::read_some(stream, buffer, parser, ec);
            if (ec == http::error::need_buffer) {
                ec = {}; // the buffer filled up: not an error, just keep reading
            }
            if (ec) {
                break; // handled below, after the decoded output is accounted for
            }
            const std::size_t n = buf.size() - parser.get().body().size;
            if (n != 0) {
                scanner.feed(std::string_view(buf.data(), n));
            }
        }
    } catch (...) {
        // A throwing consumer aborts the build read: close gracefully and let
        // the consumer's exception propagate to the build() caller.
        transport->close();
        throw;
    }
    transport->close();

    // A build error recorded in the stream beats a transport error: when the
    // daemon reset the connection right after reporting the failure, the
    // failure is the diagnosis. finish() throws it (with the output tail).
    scanner.finish();
    if (ec) {
        // The body ended early without a build error: surface the transport
        // problem (a deadline expiry becomes TransportTimeoutError).
        docker::throw_transport_error(ctx + " failed reading the build stream: " + ec.message(),
                                      ec);
    }
}

std::string DockerClient::create_container(const CreateContainerSpec& spec,
                                           const std::optional<RegistryAuth>& auth) {
    const std::string body = docker::build_create_body(spec).dump();
    const std::string target =
        versioned("/containers/create" + docker::build_create_query(spec, [](const std::string& v) {
                      return url_encode(v);
                  }));
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    Response res = request("POST", target, body, headers);
    if (res.status_code == 404) {
        // Image not present locally — pull it (with auth) and retry once.
        pull_image(spec.image, auth);
        res = request("POST", target, body, headers);
    }
    if (res.status_code != 201) {
        throw_status_error("create_container('" + spec.image + "')", res, spec.image);
    }

    return docker::expect_string_field(res.body, "Id", "create_container('" + spec.image + "')");
}

void DockerClient::start_container(const std::string& id) {
    const Response res = request("POST", versioned("/containers/" + id + "/start"));
    // 204 = started, 304 = already started.
    if (res.status_code != 204 && res.status_code != 304) {
        throw_status_error("start_container(" + id + ")", res, id);
    }
}

std::string DockerClient::inspect_container_raw(const std::string& id) {
    const Response res = request("GET", versioned("/containers/" + id + "/json"));
    if (res.status_code != 200) {
        // 404 ("no such container") becomes NotFoundError via throw_status_error.
        throw_status_error("inspect_container(" + id + ")", res, id);
    }
    return res.body;
}

ContainerInspect DockerClient::inspect_container(const std::string& id) {
    return docker::parse_inspect(inspect_container_raw(id));
}

std::vector<ContainerSummary>
DockerClient::list_containers(const std::vector<std::pair<std::string, std::string>>& label_filters,
                              bool all) {
    std::string target = versioned("/containers/json?all=");
    target += all ? "1" : "0";
    if (!label_filters.empty()) {
        // Docker's filters map each category (here "label") to an array of values;
        // a label filter's value is the "key=value" equality expression. Each pair
        // is {category, expression}, e.g. {"label", "com.docker.compose.project=tc1"}.
        nlohmann::json filters = nlohmann::json::object();
        for (const auto& [category, expression] : label_filters) {
            filters[category].push_back(expression);
        }
        target += "&filters=" + url_encode(filters.dump());
    }

    const Response res = request("GET", target);
    if (res.status_code != 200) {
        throw_status_error("list_containers", res);
    }
    return docker::parse_container_list(res.body);
}

void DockerClient::stop_container(const std::string& id, std::optional<int> timeout_secs) {
    std::string target = versioned("/containers/" + id + "/stop");
    // The daemon replies only after the container stopped — up to the full
    // grace period with zero bytes on the wire — so the io deadline must
    // outlive the grace or a long stop would read as a transport timeout.
    // Without an explicit t the daemon uses the container's StopTimeout
    // (create-time, default 10s); a negative t means "wait indefinitely".
    std::optional<std::chrono::milliseconds> io = timeouts_.io;
    if (io) {
        if (timeout_secs && *timeout_secs < 0) {
            io = std::nullopt;
        } else {
            io = std::max(*io, std::chrono::milliseconds(
                                   std::chrono::seconds(timeout_secs.value_or(10) + 30)));
        }
    }
    if (timeout_secs) {
        target += "?t=" + std::to_string(*timeout_secs);
    }
    const Response res = request_with_io_timeout("POST", target, {}, {}, io);
    // 204 = stopped, 304 = already stopped.
    if (res.status_code != 204 && res.status_code != 304) {
        throw_status_error("stop_container(" + id + ")", res, id);
    }
}

void DockerClient::remove_container(const std::string& id, bool force, bool remove_volumes) {
    const std::string target =
        versioned("/containers/" + id + "?force=" + (force ? "true" : "false") +
                  "&v=" + (remove_volumes ? "true" : "false"));
    const Response res = request("DELETE", target);
    if (res.status_code != 204) {
        throw_status_error("remove_container(" + id + ")", res, id);
    }
}

namespace {

/// Build the `GET /containers/{id}/logs` query target. `follow` is passed
/// explicitly (never taken from `opts`): the snapshot path forces it off, the
/// streaming path forces it on.
std::string build_logs_target(const std::string& id, const LogOptions& opts, bool follow) {
    return "/containers/" + id + "/logs?stdout=" + (opts.include_stdout ? "1" : "0") +
           "&stderr=" + (opts.include_stderr ? "1" : "0") + "&follow=" + (follow ? "1" : "0") +
           "&tail=" + url_encode(opts.tail) + "&timestamps=" + (opts.timestamps ? "1" : "0");
}

} // namespace

ContainerLogs DockerClient::logs(const std::string& id, const LogOptions& opts) {
    // Snapshot: follow=0 (requesting follow=1 here would block http::read until
    // the container stops). Streaming callers use follow_logs().
    const std::string target = versioned(build_logs_target(id, opts, /*follow*/ false));

    // Not const: the body / demuxed halves are moved out below.
    Response res = request("GET", target);
    if (res.status_code != 200) {
        throw_status_error("logs(" + id + ")", res, id);
    }

    ContainerLogs out;
    if (opts.tty) {
        // A TTY container's log stream is raw/unframed (no 8-byte multiplex
        // header): route it all to stdout_data verbatim, leaving stderr empty.
        out.stdout_data = std::move(res.body);
        return out;
    }

    docker::DemuxedLogs demuxed = docker::demux_all(res.body);
    out.stdout_data = std::move(demuxed.stdout_data);
    out.stderr_data = std::move(demuxed.stderr_data);
    return out;
}

void DockerClient::follow_logs(const std::string& id, const LogOptions& opts,
                               const LogConsumer& consumer) {
    follow_logs_impl(id, opts, consumer, std::nullopt);
}

FollowEnd DockerClient::follow_logs(const std::string& id, const LogOptions& opts,
                                    const LogConsumer& consumer,
                                    std::chrono::steady_clock::time_point deadline) {
    return follow_logs_impl(id, opts, consumer, deadline);
}

FollowEnd
DockerClient::follow_logs_impl(const std::string& id, const LogOptions& opts,
                               const LogConsumer& consumer,
                               std::optional<std::chrono::steady_clock::time_point> deadline) {
    // The version pin may trigger the one-time negotiation round-trip; resolve
    // it before connecting the streaming transport so that connection's clock
    // (and a caller's deadline) is not spent on an unrelated exchange.
    const std::string target = versioned(build_logs_target(id, opts, /*follow*/ true));

    auto transport = docker::connect(host_, timeouts_);
    docker::TransportStream stream{*transport};

    const auto req = make_request<http::empty_body>(http::verb::get, target, host_);

    boost::system::error_code ec;
    http::write(stream, req, ec);
    if (ec) {
        transport->close();
        docker::throw_transport_error(
            "Failed to send request to Docker (GET " + target + "): " + ec.message(), ec);
    }

    // Read incrementally with a buffer_body parser so frames are decoded as they
    // arrive instead of waiting for the (unbounded, follow) body to complete.
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);

    // The GET target carries the log options (tail/timestamps/...) — keep it in
    // the error context so a rejected logs request is diagnosable.
    read_ok_header(*transport, stream, buffer, parser,
                   "follow_logs(" + id + ") (GET " + target + ")", id);
    // Following logs legitimately idles until the container writes the next
    // line: with no deadline, waiting indefinitely is the intended behavior;
    // with one, stream_body_to_consumer re-arms the io deadline per read from
    // the remaining budget.
    if (!deadline) {
        transport->set_io_timeout(std::nullopt);
    }
    const FollowEnd end = docker::stream_body_to_consumer(*transport, stream, buffer, parser,
                                                          opts.tty, consumer, deadline);

    // Closing the connection tells Docker to stop streaming (also on early stop).
    transport->close();
    return end;
}

namespace {

/// Create an exec instance (`POST /containers/{id}/exec`) and return its id.
/// `target` is the (already version-pinned) exec-create path; `create_body` is
/// the JSON from build_exec_create_body.
std::string exec_create(DockerClient& client, const std::string& id, const std::string& target,
                        const std::string& create_body) {
    const std::vector<std::pair<std::string, std::string>> json_headers = {
        {"Content-Type", "application/json"}};
    const Response res = client.request("POST", target, create_body, json_headers);
    if (res.status_code != 201) {
        throw_status_error("exec create on container " + id, res, id);
    }
    return docker::expect_string_field(res.body, "Id", "exec create on container " + id);
}

/// Throw when `opts.stdin_data` is set but `transport` cannot half-close:
/// after writing the bytes we must signal EOF or an in-container reader like
/// `cat` blocks forever. Called BEFORE exec_create so no abandoned exec
/// instance is left on the daemon (they live until the container is removed).
void require_stdin_capable(docker::ITransport& transport, const ExecOptions& opts) {
    if (opts.stdin_data && !transport.supports_half_close()) {
        transport.close();
        throw DockerError(
            "exec: stdin_data requires a transport that can signal EOF (TCP, unix "
            "socket, or a message-mode Windows named pipe); the TLS transport (and a "
            "byte-mode pipe) cannot half-close, so the in-container reader would hang");
    }
}

/// The JSON body for `POST /exec/{exec_id}/start` — the daemon reads only
/// Detach and Tty here (everything else was fixed at exec create).
std::string build_exec_start_body(const ExecOptions& opts) {
    return std::string(R"({"Detach":)") + (opts.detach ? "true" : "false") + R"(,"Tty":)" +
           (opts.tty ? "true" : "false") + "}";
}

/// Start an already-created exec DETACHED: a plain request/response round-trip
/// (no upgrade, no hijack — `docker exec -d` parity). The daemon answers 200
/// with no stream and the command keeps running in the background.
void exec_start_detached(DockerClient& client, const std::string& exec_id,
                         const std::string& start_target, const ExecOptions& opts) {
    const std::vector<std::pair<std::string, std::string>> json_headers = {
        {"Content-Type", "application/json"}};
    const Response res =
        client.request("POST", start_target, build_exec_start_body(opts), json_headers);
    if (res.status_code != 200) {
        throw_status_error("exec start " + exec_id, res, exec_id);
    }
}

/// Open the exec start stream on an already-connected `stream`: write
/// `POST /exec/{exec_id}/start` (`start_target`, already version-pinned). On a
/// write error, closes `transport` and throws. The caller must then read the
/// response HEADER, feed stdin via `feed_stdin` (if any), and read the
/// (multiplexed or, with tty, raw) body.
void exec_start(docker::ITransport& transport, docker::TransportStream& stream,
                const DockerHost& host, const std::string& exec_id, const std::string& start_target,
                const ExecOptions& opts) {
    const std::string start_body = build_exec_start_body(opts);

    auto req = make_request<http::string_body>(http::verb::post, start_target, host);
    req.set(http::field::content_type, "application/json");
    // ALWAYS ask the daemon to switch the connection to a raw bidirectional
    // stream (101 UPGRADED) — the docker CLI runs every exec start this way,
    // stdin or not, and the road less traveled is broken in real daemons:
    // (1) HTTP-aware intermediaries (Docker Desktop's named-pipe proxy) treat
    //     client bytes sent after a completed non-upgraded POST as the start
    //     of the NEXT request and drop them — stdin and its EOF never arrive;
    // (2) the Windows-containers daemon (observed on 29.1.5) never terminates
    //     the NON-upgraded exec-start response: the output arrives but the
    //     body read then waits forever for a close that never comes (pinned
    //     by a CI hang dump; the upgraded stream IS closed on exec exit).
    // A daemon that ignores the upgrade still answers 200 and the HTTP-body
    // read path below handles it.
    req.set(http::field::connection, "Upgrade");
    req.set(http::field::upgrade, "tcp");
    req.body() = start_body;
    req.prepare_payload();

    boost::system::error_code ec;
    http::write(stream, req, ec);
    if (ec) {
        transport.close();
        docker::throw_transport_error(
            "exec start " + exec_id + " failed to send request: " + ec.message(), ec);
    }
}

/// Feed `opts.stdin_data` (when set) onto the hijacked stream, then half-close
/// the send side so the in-container reader (e.g. `cat`) terminates on EOF.
/// MUST run after the exec-start response header has been read: bytes written
/// before the daemon switches the connection to the raw stream are dropped by
/// intermediaries (Docker Desktop's named-pipe proxy loses both the data and
/// the EOF; the docker CLI also reads the response header before streaming
/// stdin). On a write error, closes `transport` and throws.
void feed_stdin(docker::ITransport& transport, docker::TransportStream& stream,
                const std::string& exec_id, const ExecOptions& opts) {
    if (!opts.stdin_data) {
        return;
    }
    boost::system::error_code ec;
    const std::string& in = *opts.stdin_data;
    std::size_t sent = 0;
    while (sent < in.size() && !ec) {
        const std::size_t n =
            stream.write_some(boost::asio::const_buffer(in.data() + sent, in.size() - sent), ec);
        if (n == 0 && !ec) {
            // Defensive: a 0-byte write without an error would spin forever.
            ec = boost::asio::error::broken_pipe;
        }
        sent += n;
    }
    if (ec) {
        transport.close();
        docker::throw_transport_error(
            "exec start " + exec_id + " failed to send stdin: " + ec.message(), ec);
    }
    transport.shutdown_send(); // signal end-of-input (EOF) to the container
}

/// Set up `transport`'s io deadline for the streaming phase of an exec (after
/// the start-response header): without a caller deadline it is DISABLED —
/// waiting indefinitely for the command's next chunk is the intended behavior
/// — and with one it is armed with the remaining budget, bounding the stdin
/// write (the read loops then re-arm per read). Returns false when the
/// deadline has already passed — the caller reports DeadlineExpired without
/// feeding stdin or reading output (the exec was started and keeps running).
bool arm_streaming_deadline(docker::ITransport& transport,
                            std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (!deadline) {
        transport.set_io_timeout(std::nullopt);
        return true;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        *deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
        return false;
    }
    transport.set_io_timeout(remaining);
    return true;
}

} // namespace

ExecResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd) {
    return exec(id, cmd, ExecOptions{});
}

ExecResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd,
                              const ExecOptions& opts) {
    if (opts.detach) {
        // Fire-and-forget: nothing is attached, so there is no stream to hijack
        // and no output or exit code to collect — two plain round-trips (create
        // + start) and the command is left running in the background.
        if (opts.stdin_data) {
            throw DockerError("exec: detach cannot be combined with stdin_data (a detached exec "
                              "attaches no streams, so there is no stdin to feed)");
        }
        const std::string exec_id = exec_create(*this, id, versioned("/containers/" + id + "/exec"),
                                                docker::build_exec_create_body(cmd, opts).dump());
        exec_start_detached(*this, exec_id, versioned("/exec/" + exec_id + "/start"), opts);
        if (opts.on_started) {
            opts.on_started(exec_id);
        }
        // The command is still running — return the defaults (empty output,
        // exit_code 0) instead of inspecting a not-yet-exited exec.
        return ExecResult{};
    }

    // The attached path IS the streaming implementation with an accumulating
    // consumer: the demuxed halves append into the result (with tty everything
    // arrives as stdout). The consumer never stops early, so delivery runs
    // until the stream ends — a daemon resetting the connection mid-stream
    // included (the output received so far is kept; the exec inspect settles
    // the outcome, matching the stdin-pump contract).
    ExecResult out;
    const ExecStreamResult res = exec_stream_impl(
        id, cmd, opts,
        [&out](LogSource source, std::string_view chunk) {
            (source == LogSource::Stderr ? out.stderr_data : out.stdout_data).append(chunk);
            return true;
        },
        std::nullopt);
    // Historical contract: a command the inspect still sees running (possible
    // only after an abnormal early stream end) reads as exit code 0.
    out.exit_code = res.exit_code.value_or(0);
    return out;
}

ExecResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd,
                              const ExecOptions& opts, const LogConsumer& consumer) {
    const ExecStreamResult res = exec_stream_impl(id, cmd, opts, consumer, std::nullopt);
    ExecResult out;
    // Contract preserved from before the deadline overload existed: a command
    // still running after an early consumer stop reads as exit code 0.
    out.exit_code = res.exit_code.value_or(0);
    return out;
}

ExecStreamResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd,
                                    const ExecOptions& opts, const LogConsumer& consumer,
                                    std::chrono::steady_clock::time_point deadline) {
    return exec_stream_impl(id, cmd, opts, consumer, deadline);
}

ExecStreamResult
DockerClient::exec_stream_impl(const std::string& id, const std::vector<std::string>& cmd,
                               const ExecOptions& opts, const LogConsumer& consumer,
                               std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (opts.detach) {
        throw DockerError("exec: detach cannot be combined with an output consumer (a "
                          "detached exec attaches no streams, so no output would ever arrive)");
    }

    // 1) Connect + capability check first (see the non-streaming overload).
    auto transport = docker::connect(host_, timeouts_);
    require_stdin_capable(*transport, opts);

    // 2) Create the exec instance.
    const std::string exec_id = exec_create(*this, id, versioned("/containers/" + id + "/exec"),
                                            docker::build_exec_create_body(cmd, opts).dump());

    // 3) Start the exec over the raw stream so output can be consumed
    //    incrementally (and stdin hijacked after the header — see feed_stdin).
    docker::TransportStream stream{*transport};
    exec_start(*transport, stream, host_, exec_id, versioned("/exec/" + exec_id + "/start"), opts);

    // 4) Read the response incrementally, delivering chunks to consumer as they
    //    arrive (mirrors follow_logs). With Tty=false demux frames; with Tty=true
    //    deliver the raw bytes as a single stdout stream. Stdin on the upgraded
    //    stream is INTERLEAVED with the output read (see pump_exec_stream);
    //    everything after the header is bounded by the deadline when one was
    //    given.
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);

    read_ok_header(*transport, stream, buffer, parser, "exec start " + exec_id, exec_id,
                   /*accept_upgraded=*/true);
    // The exec is now started and the stream established: the first moment a
    // resize_exec is valid, and before any stdin/output moves.
    if (opts.on_started) {
        opts.on_started(exec_id);
    }
    FollowEnd end = FollowEnd::DeadlineExpired; // when already past the deadline
    if (opts.stdin_data && parser.get().result_int() == 101) {
        if (deadline) {
            // The absolute deadline is the pump's only bound — a still-armed
            // io deadline would masquerade as DeadlineExpired.
            transport->set_io_timeout(std::nullopt);
        }
        const auto leftover = buffer.data();
        boost::system::error_code wedge_ec;
        end = docker::pump_exec_stream(
            *transport,
            std::string_view(static_cast<const char*>(leftover.data()), leftover.size()),
            *opts.stdin_data, opts.tty, consumer, deadline, wedge_ec);
        if (wedge_ec) {
            transport->close();
            docker::throw_transport_error("exec start " + exec_id +
                                              " failed to pump stdin/output: " + wedge_ec.message(),
                                          wedge_ec);
        }
    } else if (arm_streaming_deadline(*transport, deadline)) {
        // No stdin on this path (or a daemon that ignored the upgrade, where
        // stdin — when any — goes in sequentially; see feed_stdin).
        feed_stdin(*transport, stream, exec_id, opts);
        if (parser.get().result_int() == 101) {
            // Upgraded (the normal path): raw stream, not an HTTP body (see the
            // non-streaming overload).
            const auto leftover = buffer.data();
            end = docker::stream_raw_to_consumer(
                *transport,
                std::string_view(static_cast<const char*>(leftover.data()), leftover.size()),
                opts.tty, consumer, deadline);
        } else {
            end = docker::stream_body_to_consumer(*transport, stream, buffer, parser, opts.tty,
                                                  consumer, deadline);
        }
    }
    transport->close();

    // 5) Read back how the exec ended (on a fresh connection under the normal
    //    io deadline — the caller's deadline does not bound this round-trip).
    //    Closing the attach stream does not kill the command: after an early
    //    stop (consumer / deadline) it usually KEEPS RUNNING, so the exit code
    //    is reported only when the inspect says it actually finished. The
    //    output went to consumer, so stdout_data / stderr_data have no analog.
    const Response inspect_res = request("GET", versioned("/exec/" + exec_id + "/json"));
    if (inspect_res.status_code != 200) {
        throw_status_error("exec inspect " + exec_id, inspect_res, exec_id);
    }
    ExecStreamResult out;
    out.end = end;
    if (const docker::ExecStatus status = docker::parse_exec_status(inspect_res.body);
        !status.running) {
        out.exit_code = status.exit_code;
    }
    return out;
}

void DockerClient::resize_exec(const std::string& exec_id, TtySize size) {
    const Response res =
        request("POST", versioned("/exec/" + exec_id + "/resize?h=" + std::to_string(size.height) +
                                  "&w=" + std::to_string(size.width)));
    if (!res.ok()) {
        throw_status_error("exec resize " + exec_id, res, exec_id);
    }
}

void DockerClient::resize_container_tty(const std::string& id, TtySize size) {
    const Response res =
        request("POST", versioned("/containers/" + id + "/resize?h=" + std::to_string(size.height) +
                                  "&w=" + std::to_string(size.width)));
    if (!res.ok()) {
        throw_status_error("container tty resize " + id, res, id);
    }
}

void DockerClient::copy_to_container(const std::string& id, const CopyToContainer& source) {
    copy_to_archive(id, "copy_to_container(" + id + ", '" + source.target() + "')",
                    [&source](const docker::ByteSink& sink) { docker::stream_tar(source, sink); });
}

void DockerClient::copy_to_container(const std::string& id,
                                     const std::vector<CopyToContainer>& sources) {
    if (sources.empty()) {
        return; // nothing to copy: skip the round-trip (and the version ping)
    }
    copy_to_archive(
        id, "copy_to_container(" + id + ", " + std::to_string(sources.size()) + " sources)",
        [&sources](const docker::ByteSink& sink) { docker::stream_tar(sources, sink); });
}

void DockerClient::copy_to_archive(const std::string& id, const std::string& context,
                                   const docker::BodyProducer& producer) {
    // Always extract at the root with relative entry names (stream_tar
    // normalizes each target — leading '/' stripped, Windows drive prefixes
    // dropped). A single-file source needs its parent directory to already
    // exist; a directory source ships its own directory entries. The version
    // pin may trigger the one-time negotiation round-trip; resolve it before
    // connecting the upload transport.
    const std::string target =
        versioned("/containers/" + id + "/archive?path=/&noOverwriteDirNonDir=false");

    auto transport = docker::connect(host_, timeouts_);
    docker::TransportStream stream{*transport};

    auto req = make_request<http::buffer_body>(http::verb::put, target, host_);
    req.set(http::field::content_type, "application/x-tar");
    write_chunked_request(*transport, stream, req, producer, context, id);

    // The reply is small: an empty 200 or an error JSON.
    boost::system::error_code ec;
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(boost::none);
    http::read(stream, buffer, parser, ec);
    if (ec == http::error::end_of_stream && !parser.is_done()) {
        transport->close();
        throw DockerError(context + " failed: connection closed before the response completed");
    }
    if (ec && ec != http::error::end_of_stream) {
        transport->close();
        docker::throw_transport_error(context + " failed to read response: " + ec.message(), ec);
    }
    transport->close();
    if (parser.get().result_int() != 200) {
        throw_status_error(context, static_cast<int>(parser.get().result_int()),
                           parser.get().body(), id);
    }
}

std::string DockerClient::copy_from_container(const std::string& id,
                                              const std::string& container_path) {
    const std::string target =
        versioned("/containers/" + id + "/archive?path=" + url_encode(container_path));

    const Response res = request("GET", target);
    if (res.status_code == 200) {
        return res.body;
    }
    if (res.status_code == 404) {
        throw NotFoundError("copy_from_container(" + id + ", '" + container_path +
                                "') failed: no such container or path: HTTP 404 " + res.body,
                            id);
    }
    throw_status_error("copy_from_container(" + id + ", '" + container_path + "')", res, id);
}

namespace {

/// An open, streaming `GET /containers/{id}/archive` download: read_block()
/// hands out successive body blocks — the pull shape both the sink overload
/// and libarchive's read callback need. The constructor sends the request
/// and requires a 200 header (a 404 throws NotFoundError before any block);
/// the destructor closes the transport (idempotent), so an aborted consumer
/// unwinds cleanly.
class ArchiveDownload {
public:
    ArchiveDownload(const DockerHost& host, const docker::TransportTimeouts& timeouts,
                    const std::string& target, std::string context, const std::string& id)
        : transport_(docker::connect(host, timeouts)), stream_(*transport_),
          context_(std::move(context)) {
        const auto req = make_request<http::empty_body>(http::verb::get, target, host);
        boost::system::error_code ec;
        http::write(stream_, req, ec);
        if (ec) {
            transport_->close();
            docker::throw_transport_error(context_ + " failed to send request: " + ec.message(),
                                          ec);
        }
        parser_.body_limit(boost::none);
        read_ok_header(*transport_, stream_, buffer_, parser_, context_, id);
    }
    ~ArchiveDownload() { transport_->close(); }
    ArchiveDownload(const ArchiveDownload&) = delete;
    ArchiveDownload& operator=(const ArchiveDownload&) = delete;

    /// Read the next body block into data[0..size); 0 = end of the archive.
    std::size_t read_block(char* data, std::size_t size) {
        while (!parser_.is_done()) {
            parser_.get().body().data = data;
            parser_.get().body().size = size;
            boost::system::error_code ec;
            http::read_some(stream_, buffer_, parser_, ec);
            if (ec == http::error::need_buffer) {
                ec = {};
            }
            if (ec == http::error::end_of_stream && parser_.is_done()) {
                ec = {}; // EOF-delimited body ended exactly here: normal
            }
            if (ec) {
                transport_->close();
                docker::throw_transport_error(
                    context_ + " failed reading the archive stream: " + ec.message(), ec);
            }
            const std::size_t got = size - parser_.get().body().size;
            if (got != 0) {
                return got;
            }
            // A zero-byte pass parses chunk framing only — read again.
        }
        return 0;
    }

private:
    std::shared_ptr<docker::ITransport> transport_;
    docker::TransportStream stream_;
    boost::beast::flat_buffer buffer_;
    http::response_parser<http::buffer_body> parser_;
    std::string context_;
};

} // namespace

void DockerClient::copy_from_container(const std::string& id, const std::string& container_path,
                                       const docker::ByteSink& sink) {
    const std::string target =
        versioned("/containers/" + id + "/archive?path=" + url_encode(container_path));
    ArchiveDownload download(host_, timeouts_, target,
                             "copy_from_container(" + id + ", '" + container_path + "')", id);

    std::vector<char> block(std::size_t{64} * 1024);
    for (;;) {
        const std::size_t n = download.read_block(block.data(), block.size());
        if (n == 0) {
            return;
        }
        sink(block.data(), n);
    }
}

void DockerClient::copy_from_container_to(const std::string& id, const std::string& container_path,
                                          const std::filesystem::path& dest_dir) {
    const std::string target =
        versioned("/containers/" + id + "/archive?path=" + url_encode(container_path));
    ArchiveDownload download(host_, timeouts_, target,
                             "copy_from_container_to(" + id + ", '" + container_path + "')", id);

    // libarchive pulls blocks straight off the response as it extracts —
    // wire to disk, nothing buffered whole.
    docker::extract_tar_to_dir(
        [&download](char* data, std::size_t size) { return download.read_block(data, size); },
        dest_dir);
}

DockerClient::ContainerPathStat
DockerClient::container_path_stat(const std::string& id, const std::string& container_path) {
    const std::string target =
        versioned("/containers/" + id + "/archive?path=" + url_encode(container_path));
    const std::string context = "container_path_stat(" + id + ", '" + container_path + "')";

    const Response res = request("HEAD", target);
    if (res.status_code != 200) {
        throw_status_error(context, res, id);
    }
    const std::string encoded = res.header("X-Docker-Container-Path-Stat");
    if (encoded.empty()) {
        throw DockerError(context + " failed: the daemon sent no X-Docker-Container-Path-Stat "
                                    "header");
    }

    ContainerPathStat out;
    try {
        const nlohmann::json stat = nlohmann::json::parse(docker::base64_decode(encoded));
        out.name = stat.value("name", "");
        out.size = stat.value("size", std::uint64_t{0});
        out.mode = stat.value("mode", std::uint32_t{0});
        out.mtime = stat.value("mtime", "");
        out.link_target = stat.value("linkTarget", "");
    } catch (const std::exception& e) {
        throw DockerError(context +
                          " failed: undecodable X-Docker-Container-Path-Stat header: " + e.what());
    }
    // Go's os.FileMode keeps the type bits high; bit 31 is ModeDir.
    out.is_dir = (out.mode & 0x80000000U) != 0;
    return out;
}

std::string
DockerClient::create_network(const std::string& name,
                             const std::vector<std::pair<std::string, std::string>>& labels) {
    // A name+labels network is just a minimal spec; one construction path.
    NetworkCreateSpec spec;
    spec.name = name;
    spec.labels = labels;
    return create_network(spec);
}

std::string DockerClient::create_network(const NetworkCreateSpec& spec) {
    const std::string body = docker::build_network_create_body(spec).dump();
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    const Response res = request("POST", versioned("/networks/create"), body, headers);
    if (res.status_code != 201) {
        throw_status_error("create_network('" + spec.name + "')", res, spec.name);
    }
    return docker::expect_string_field(res.body, "Id", "create_network('" + spec.name + "')");
}

std::string DockerClient::inspect_network_raw(const std::string& id) {
    const Response res = request("GET", versioned("/networks/" + id));
    if (res.status_code != 200) {
        // 404 ("no such network") becomes NotFoundError via throw_status_error.
        throw_status_error("inspect_network(" + id + ")", res, id);
    }
    return res.body;
}

NetworkInspect DockerClient::inspect_network(const std::string& id) {
    return docker::parse_network_inspect(inspect_network_raw(id));
}

void DockerClient::connect_network(const std::string& network_id, const std::string& container_id,
                                   const std::vector<std::string>& aliases) {
    const std::string body = docker::build_connect_network_body(container_id, aliases).dump();
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    const Response res =
        request("POST", versioned("/networks/" + network_id + "/connect"), body, headers);
    // 200 (older daemons) or 204 (current) both mean connected.
    if (res.status_code != 200 && res.status_code != 204) {
        throw_status_error("connect_network(" + network_id + ", " + container_id + ")", res,
                           network_id);
    }
}

void DockerClient::disconnect_network(const std::string& network_id,
                                      const std::string& container_id, bool force) {
    nlohmann::json body;
    body["Container"] = container_id;
    body["Force"] = force;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    const Response res =
        request("POST", versioned("/networks/" + network_id + "/disconnect"), body.dump(), headers);
    // 200 (older daemons) or 204 (current) both mean disconnected.
    if (res.status_code != 200 && res.status_code != 204) {
        throw_status_error("disconnect_network(" + network_id + ", " + container_id + ")", res,
                           network_id);
    }
}

void DockerClient::remove_network(const std::string& id) {
    const Response res = request("DELETE", versioned("/networks/" + id));
    if (res.status_code != 204) {
        throw_status_error("remove_network(" + id + ")", res, id);
    }
}

std::string DockerClient::create_volume(const VolumeCreateSpec& spec) {
    const std::string body = docker::build_volume_create_body(spec).dump();
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    const Response res = request("POST", versioned("/volumes/create"), body, headers);
    if (res.status_code != 201) {
        throw_status_error("create_volume('" + spec.name + "')", res, spec.name);
    }
    return docker::expect_string_field(res.body, "Name", "create_volume('" + spec.name + "')");
}

VolumeInspect DockerClient::inspect_volume(const std::string& name) {
    const Response res = request("GET", versioned("/volumes/" + url_encode(name)));
    if (res.status_code != 200) {
        // 404 ("no such volume") becomes NotFoundError via throw_status_error.
        throw_status_error("inspect_volume('" + name + "')", res, name);
    }
    return docker::parse_volume_inspect(res.body);
}

void DockerClient::remove_volume(const std::string& name, bool force) {
    const std::string target =
        versioned("/volumes/" + url_encode(name) + "?force=" + (force ? "true" : "false"));
    const Response res = request("DELETE", target);
    if (res.status_code != 204) {
        // 404 (absent, -> NotFoundError) or 409 (still in use) both arrive here.
        throw_status_error("remove_volume('" + name + "')", res, name);
    }
}

} // namespace testcontainers
