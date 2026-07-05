#include "testcontainers/docker/DockerClient.hpp"

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
#include <boost/beast/http.hpp>
#include <boost/optional.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

namespace testcontainers {

namespace {

namespace http = boost::beast::http;

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

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

inline std::string read_error_body(docker::TransportStream& stream,
                                   boost::beast::flat_buffer& buffer,
                                   http::response_parser<http::string_body>& parser) {
    boost::system::error_code ec;
    http::read(stream, buffer, parser, ec); // error bodies are small JSON lines
    if (ec && ec != http::error::end_of_stream) {
        return {};
    }
    std::string body = std::move(parser.get().body());
    if (body.size() > 8192) {
        body.resize(8192);
    }
    return body;
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

/// Read the response header off a hijacked/streaming connection and require
/// HTTP 200 (or, with `accept_upgraded`, the 101 a connection-upgrade request
/// is answered with): on a read error, an immediate EOF, or a failing status
/// this closes `transport` and throws the typed error prefixed with `context`
/// (a deadline expiry becomes TransportTimeoutError, a bad status goes through
/// throw_status_error with the daemon's drained error body appended).
template <class Parser>
void read_ok_header(docker::ITransport& transport, docker::TransportStream& stream,
                    boost::beast::flat_buffer& buffer, Parser& parser, const std::string& context,
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
        if (iequals(key, name)) {
            return value;
        }
    }
    return {};
}

DockerClient::DockerClient(DockerHost host) : host_(std::move(host)) {}

DockerClient DockerClient::from_environment() { return DockerClient(DockerHost::resolve()); }

const std::string& DockerClient::api_prefix() {
    if (!api_prefix_) {
        // One unversioned round-trip; the daemon's version middleware stamps
        // its newest supported version on the reply. A daemon that answers
        // without a parsable Api-Version (or a non-2xx) is cached as "no pin":
        // unversioned paths keep working against its default version. A
        // TRANSPORT failure propagates uncached — the call that triggered the
        // negotiation would have failed the same way, and a later retry gets a
        // fresh chance to negotiate.
        const Response res = request("GET", "/_ping");
        const std::string negotiated = docker::negotiate_api_version(res.header("Api-Version"));
        api_prefix_ = negotiated.empty() ? std::string{} : "/v" + negotiated;
    }
    return *api_prefix_;
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
        // Disable Beast's default 1 MiB body limit: Docker archive (copy-from)
        // and log bodies can comfortably exceed it, and we always read the
        // whole body.
        http::response_parser<http::string_body> parser;
        parser.body_limit(boost::none);
        http::read(stream, buffer, parser, ec);
        if (ec == http::error::end_of_stream && !parser.is_done()) {
            // EOF before a complete response. Without this check a stale
            // kept-alive connection (peer closed it while idle) would read a
            // never-populated parser and masquerade as an empty success.
            throw DockerError("Failed to read response from Docker (" + std::string(method) + " " +
                              std::string(target) +
                              "): connection closed before the response completed");
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
    // The engine mode (Linux vs Windows containers) is fixed for the life of the
    // process, so the first successful answer is cached and reused. Guarded by a
    // mutex because a process may share the daemon across threads.
    static std::mutex cache_mutex;
    static std::optional<std::string> cached;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cached) {
            return *cached;
        }
    }

    const Response res = request("GET", versioned("/version"));
    if (!res.ok()) {
        throw_status_error("server_os: GET /version", res);
    }
    std::string os = docker::parse_server_os(res.body);

    std::lock_guard<std::mutex> lock(cache_mutex);
    cached = os;
    return os;
}

bool DockerClient::is_windows_engine() {
    const std::string os = server_os();
    // Case-insensitive "contains windows".
    std::string lower;
    lower.reserve(os.size());
    for (const unsigned char c : os) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower.find("windows") != std::string::npos;
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

    const Response res = request("POST", target, /*body*/ {}, headers);
    if (res.status_code != 200) {
        throw_status_error("pull_image('" + image + "')", res, image);
    }
    // Docker streams progress as newline-delimited JSON and returns 200 even on
    // failure, embedding the error in the stream.
    docker::throw_if_pull_error(res.body, image);
}

void DockerClient::build_image(const std::string& context_tar,
                               const docker::BuildOptions& options) {
    const std::string target =
        versioned("/build" + docker::build_build_query(
                                 options, [](const std::string& v) { return url_encode(v); }));
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-tar"}};

    // The legacy /build endpoint streams output only when a step produces it —
    // a silent RUN step can legitimately idle for minutes — so widen the io
    // deadline instead of misreading a quiet build as a wedged daemon.
    std::optional<std::chrono::milliseconds> io = timeouts_.io;
    if (io) {
        io = std::max(*io, std::chrono::milliseconds(std::chrono::minutes(10)));
    }
    const Response res = request_with_io_timeout("POST", target, context_tar, headers, io);
    if (res.status_code != 200) {
        throw_status_error("build_image('" + options.tag + "')", res, options.tag);
    }
    // Docker streams build output as newline-delimited JSON and returns 200 even
    // on a build failure, embedding the error in the stream.
    docker::throw_if_build_error(res.body, options.tag);
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

/// Open the exec start stream on an already-connected `stream`: write
/// `POST /exec/{exec_id}/start` (`start_target`, already version-pinned). On a
/// write error, closes `transport` and throws. The caller must then read the
/// response HEADER, feed stdin via `feed_stdin` (if any), and read the
/// (multiplexed or, with tty, raw) body.
void exec_start(docker::ITransport& transport, docker::TransportStream& stream,
                const DockerHost& host, const std::string& exec_id, const std::string& start_target,
                const ExecOptions& opts) {
    const std::string start_body =
        std::string(R"({"Detach":false,"Tty":)") + (opts.tty ? "true" : "false") + "}";

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

} // namespace

ExecResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd) {
    return exec(id, cmd, ExecOptions{});
}

ExecResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd,
                              const ExecOptions& opts) {
    // 1) Connect the raw transport first and check the stdin capability BEFORE
    //    creating the exec instance, so an unsupported-transport failure leaves
    //    no abandoned exec on the daemon.
    auto transport = docker::connect(host_, timeouts_);
    require_stdin_capable(*transport, opts);

    // 2) Create the exec instance (carrying env / workdir / user / privileged /
    //    tty / attach-stdin from opts).
    const std::string exec_id = exec_create(*this, id, versioned("/containers/" + id + "/exec"),
                                            docker::build_exec_create_body(cmd, opts).dump());

    // 3) Start the exec over the raw transport stream so stdin can (when
    //    requested) be hijacked after the response header (see feed_stdin).
    docker::TransportStream stream{*transport};
    exec_start(*transport, stream, host_, exec_id, versioned("/exec/" + exec_id + "/start"), opts);

    // 4) Read the whole start response. With Tty=false this is the multiplexed
    //    frame stream (demux it); with Tty=true it is raw, unframed bytes.
    boost::system::error_code ec;
    boost::beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(boost::none);
    read_ok_header(*transport, stream, buffer, parser, "exec start " + exec_id, exec_id,
                   /*accept_upgraded=*/true);
    // Stdin goes in only AFTER the response header: the connection is now the
    // raw hijacked stream end to end (see feed_stdin).
    feed_stdin(*transport, stream, exec_id, opts);
    // The output completes only when the command exits — it may legitimately
    // run (and stay silent) for as long as the caller's command takes.
    transport->set_io_timeout(std::nullopt);
    std::string body;
    if (parser.get().result_int() == 101) {
        // Upgraded (the normal path): the exec stream arrives raw on the
        // connection, not as an HTTP body; the header parse may already have
        // pulled some of it into `buffer`.
        const auto leftover = buffer.data();
        body = docker::read_raw_stream(
            *transport,
            std::string_view(static_cast<const char*>(leftover.data()), leftover.size()), ec);
        if (ec) {
            transport->close();
            docker::throw_transport_error(
                "exec start " + exec_id + " failed to read the upgraded stream: " + ec.message(),
                ec);
        }
    } else {
        http::read(stream, buffer, parser, ec); // the rest of the body
        if (ec && ec != http::error::end_of_stream) {
            transport->close();
            docker::throw_transport_error(
                "exec start " + exec_id + " failed to read response: " + ec.message(), ec);
        }
        body = std::move(parser.get().body());
    }
    transport->close();

    ExecResult out;
    if (opts.tty) {
        // Raw, unframed single stream: route it all to stdout_data unchanged.
        out.stdout_data = std::move(body);
    } else {
        // Not const: the demuxed halves are moved out.
        docker::DemuxedLogs demuxed = docker::demux_all(body);
        out.stdout_data = std::move(demuxed.stdout_data);
        out.stderr_data = std::move(demuxed.stderr_data);
    }

    // 5) Inspect the exec for the exit code.
    const Response inspect_res = request("GET", versioned("/exec/" + exec_id + "/json"));
    if (inspect_res.status_code != 200) {
        throw_status_error("exec inspect " + exec_id, inspect_res, exec_id);
    }
    out.exit_code = docker::parse_exec_exit_code(inspect_res.body);
    return out;
}

ExecResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd,
                              const ExecOptions& opts, const LogConsumer& consumer) {
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
    //    deliver the raw bytes as a single stdout stream.
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);

    read_ok_header(*transport, stream, buffer, parser, "exec start " + exec_id, exec_id,
                   /*accept_upgraded=*/true);
    // Stdin goes in only AFTER the response header (see feed_stdin).
    feed_stdin(*transport, stream, exec_id, opts);
    // Streaming exec output idles until the command writes the next chunk —
    // waiting indefinitely is the intended behavior from here on.
    transport->set_io_timeout(std::nullopt);
    if (parser.get().result_int() == 101) {
        // Upgraded (the normal path): raw stream, not an HTTP body (see the
        // non-streaming overload).
        const auto leftover = buffer.data();
        docker::stream_raw_to_consumer(
            *transport,
            std::string_view(static_cast<const char*>(leftover.data()), leftover.size()), opts.tty,
            consumer);
    } else {
        docker::stream_body_to_consumer(*transport, stream, buffer, parser, opts.tty, consumer);
    }
    transport->close();

    // 5) Inspect the exec for the exit code; the output went to consumer, so
    //    stdout_data / stderr_data are left empty.
    const Response inspect_res = request("GET", versioned("/exec/" + exec_id + "/json"));
    if (inspect_res.status_code != 200) {
        throw_status_error("exec inspect " + exec_id, inspect_res, exec_id);
    }
    ExecResult out;
    out.exit_code = docker::parse_exec_exit_code(inspect_res.body);
    return out;
}

void DockerClient::copy_to_container(const std::string& id, const CopyToContainer& source) {
    const std::string tar = docker::build_tar(source);
    // Always extract at the root with relative entry names (build_tar strips the
    // leading '/'), so the target's parent directory must already exist.
    const std::string target =
        versioned("/containers/" + id + "/archive?path=/&noOverwriteDirNonDir=false");
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-tar"}};

    const Response res = request("PUT", target, tar, headers);
    if (res.status_code != 200) {
        throw_status_error("copy_to_container(" + id + ", '" + source.target() + "')", res, id);
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
