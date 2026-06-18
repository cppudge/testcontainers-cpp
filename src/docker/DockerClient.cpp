#include "testcontainers/docker/DockerClient.hpp"

#include "docker/ApiMapping.hpp"
#include "docker/Auth.hpp"
#include "docker/LogDemux.hpp"
#include "docker/Tar.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/version.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/optional.hpp>

#include <array>
#include <cctype>
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

DockerClient DockerClient::from_environment() {
    return DockerClient(DockerHost::resolve());
}

Response DockerClient::request(std::string_view method, std::string_view target,
                               std::string_view body,
                               const std::vector<std::pair<std::string, std::string>>& headers) {
    auto transport = docker::connect(host_);
    docker::TransportStream stream{*transport};

    http::request<http::string_body> req{http::string_to_verb(std::string(method)),
                                         std::string(target), /*HTTP*/ 11};
    req.set(http::field::host, host_.http_host());
    req.set(http::field::user_agent, "testcontainers-cpp/" + version());
    req.keep_alive(false); // one connection per request for now
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
        throw DockerError("Failed to send request to Docker (" + std::string(method) + " " +
                          std::string(target) + "): " + ec.message());
    }

    boost::beast::flat_buffer buffer;
    // Disable Beast's default 1 MiB body limit: Docker archive (copy-from) and
    // log bodies can comfortably exceed it, and we always read the whole body.
    http::response_parser<http::string_body> parser;
    parser.body_limit(boost::none);
    http::read(stream, buffer, parser, ec);
    if (ec && ec != http::error::end_of_stream) {
        throw DockerError("Failed to read response from Docker (" + std::string(method) + " " +
                          std::string(target) + "): " + ec.message());
    }
    auto& res = parser.get();

    Response out;
    out.status_code = static_cast<int>(res.result_int());
    out.reason = std::string(res.reason());
    out.body = std::move(res.body());
    for (const auto& field : res) {
        out.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));
    }

    transport->close();
    return out;
}

bool DockerClient::ping() {
    return request("GET", "/_ping").ok();
}

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

    const Response res = request("GET", "/version");
    if (!res.ok()) {
        throw DockerError("server_os: GET /version failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
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
        "/images/create?fromImage=" + url_encode(name) + "&tag=" + url_encode(tag);

    // Use the explicit credentials if given, else auto-resolve from the Docker
    // config for this image's registry. No credentials -> a plain public pull.
    const std::optional<RegistryAuth> cred =
        auth ? auth : docker::resolve_auth_for_image(image);
    std::vector<std::pair<std::string, std::string>> headers;
    if (cred) {
        headers.emplace_back("X-Registry-Auth", docker::encode_x_registry_auth(*cred));
    }

    const Response res = request("POST", target, /*body*/ {}, headers);
    if (res.status_code != 200) {
        throw DockerError("Failed to pull image '" + image + "': HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
    // Docker streams progress as newline-delimited JSON and returns 200 even on
    // failure, embedding the error in the stream.
    docker::throw_if_pull_error(res.body, image);
}

std::string DockerClient::create_container(const CreateContainerSpec& spec,
                                           const std::optional<RegistryAuth>& auth) {
    const std::string body = docker::build_create_body(spec).dump();
    const std::string target =
        "/containers/create" +
        docker::build_create_query(spec, [](const std::string& v) { return url_encode(v); });
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    Response res = request("POST", target, body, headers);
    if (res.status_code == 404) {
        // Image not present locally — pull it (with auth) and retry once.
        pull_image(spec.image, auth);
        res = request("POST", target, body, headers);
    }
    if (res.status_code != 201) {
        throw DockerError("create_container('" + spec.image + "') failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }

    const auto json = nlohmann::json::parse(res.body);
    return json.at("Id").get<std::string>();
}

void DockerClient::start_container(const std::string& id) {
    const Response res = request("POST", "/containers/" + id + "/start");
    // 204 = started, 304 = already started.
    if (res.status_code != 204 && res.status_code != 304) {
        throw DockerError("start_container(" + id + ") failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
}

ContainerInspect DockerClient::inspect_container(const std::string& id) {
    const Response res = request("GET", "/containers/" + id + "/json");
    if (res.status_code == 404) {
        throw DockerError("Container not found: " + id);
    }
    if (res.status_code != 200) {
        throw DockerError("inspect_container(" + id + ") failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
    return docker::parse_inspect(res.body);
}

void DockerClient::stop_container(const std::string& id, std::optional<int> timeout_secs) {
    std::string target = "/containers/" + id + "/stop";
    if (timeout_secs) {
        target += "?t=" + std::to_string(*timeout_secs);
    }
    const Response res = request("POST", target);
    // 204 = stopped, 304 = already stopped.
    if (res.status_code != 204 && res.status_code != 304) {
        throw DockerError("stop_container(" + id + ") failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
}

void DockerClient::remove_container(const std::string& id, bool force, bool remove_volumes) {
    const std::string target = "/containers/" + id + "?force=" + (force ? "true" : "false") +
                               "&v=" + (remove_volumes ? "true" : "false");
    const Response res = request("DELETE", target);
    if (res.status_code != 204) {
        throw DockerError("remove_container(" + id + ") failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
}

namespace {

/// Build the `GET /containers/{id}/logs` query target. `follow` is passed
/// explicitly so the snapshot path can use `opts.follow` while the streaming
/// path forces it on.
std::string build_logs_target(const std::string& id, const LogOptions& opts, bool follow) {
    return "/containers/" + id + "/logs?stdout=" + (opts.include_stdout ? "1" : "0") +
           "&stderr=" + (opts.include_stderr ? "1" : "0") + "&follow=" + (follow ? "1" : "0") +
           "&tail=" + url_encode(opts.tail) + "&timestamps=" + (opts.timestamps ? "1" : "0");
}

} // namespace

ContainerLogs DockerClient::logs(const std::string& id, const LogOptions& opts) {
    const std::string target = build_logs_target(id, opts, opts.follow);

    const Response res = request("GET", target);
    if (res.status_code != 200) {
        throw DockerError("logs(" + id + ") failed: HTTP " + std::to_string(res.status_code) +
                          " " + res.body);
    }

    const docker::DemuxedLogs demuxed = docker::demux_all(res.body);
    ContainerLogs out;
    out.stdout_data = std::move(demuxed.stdout_data);
    out.stderr_data = std::move(demuxed.stderr_data);
    return out;
}

void DockerClient::follow_logs(const std::string& id, const LogOptions& opts,
                               const LogConsumer& consumer) {
    auto transport = docker::connect(host_);
    docker::TransportStream stream{*transport};

    // follow is forced on regardless of opts.follow; everything else mirrors logs().
    const std::string target = build_logs_target(id, opts, /*follow*/ true);

    http::request<http::empty_body> req{http::verb::get, target, /*HTTP*/ 11};
    req.set(http::field::host, host_.http_host());
    req.set(http::field::user_agent, "testcontainers-cpp/" + version());
    req.keep_alive(false); // one connection per request for now

    boost::system::error_code ec;
    http::write(stream, req, ec);
    if (ec) {
        throw DockerError("Failed to send request to Docker (GET " + target + "): " + ec.message());
    }

    // Read incrementally with a buffer_body parser so frames are decoded as they
    // arrive instead of waiting for the (unbounded, follow) body to complete.
    boost::beast::flat_buffer buffer;
    http::response_parser<http::buffer_body> parser;
    parser.body_limit(boost::none);

    http::read_header(stream, buffer, parser, ec);
    if (ec && ec != http::error::end_of_stream) {
        transport->close();
        throw DockerError("Failed to read response from Docker (GET " + target +
                          "): " + ec.message());
    }
    if (parser.get().result_int() != 200) {
        const int status = static_cast<int>(parser.get().result_int());
        transport->close();
        throw DockerError("follow_logs(" + id + ") failed: HTTP " + std::to_string(status));
    }

    docker::LogDemuxer demuxer;
    std::array<char, 8192> buf{};
    bool stop = false;
    while (!stop && !parser.is_done()) {
        parser.get().body().data = buf.data();
        parser.get().body().size = buf.size();

        // read_some (not read): read returns only when the whole message is
        // complete, which for a follow stream means "when the container stops" —
        // that would batch all output to the end. read_some returns after each
        // socket read, so frames are delivered as the daemon flushes them.
        http::read_some(stream, buffer, parser, ec);
        if (ec == http::error::need_buffer) {
            ec = {}; // the buffer filled up: not an error, just keep reading
        }
        if (ec == http::error::end_of_stream) {
            break; // the container's log stream ended (it stopped)
        }
        if (ec) {
            break; // the stream was reset/closed by the daemon; treat as end
        }

        const std::size_t n = buf.size() - parser.get().body().size;
        if (n == 0) {
            continue;
        }
        for (const auto& frame : demuxer.feed(std::string_view(buf.data(), n))) {
            LogSource source = LogSource::Stdout;
            switch (frame.stream) {
            case docker::LogStreamKind::StdIn:
                continue; // never present in log output
            case docker::LogStreamKind::StdOut:
                source = LogSource::Stdout;
                break;
            case docker::LogStreamKind::StdErr:
                source = LogSource::Stderr;
                break;
            }
            if (!consumer(source, frame.data)) {
                stop = true; // consumer asked to stop; close the connection below
                break;
            }
        }
    }

    // Closing the connection tells Docker to stop streaming (also on early stop).
    transport->close();
}

ExecResult DockerClient::exec(const std::string& id, const std::vector<std::string>& cmd) {
    const std::vector<std::pair<std::string, std::string>> json_headers = {
        {"Content-Type", "application/json"}};

    // 1) Create the exec instance.
    const std::string create_body = docker::build_exec_create_body(cmd).dump();
    const Response create_res =
        request("POST", "/containers/" + id + "/exec", create_body, json_headers);
    if (create_res.status_code != 201) {
        throw DockerError("exec create on container " + id + " failed: HTTP " +
                          std::to_string(create_res.status_code) + " " + create_res.body);
    }
    const std::string exec_id =
        nlohmann::json::parse(create_res.body).at("Id").get<std::string>();

    // 2) Start the exec; with Tty=false the body is the multiplexed stream.
    const std::string start_body = R"({"Detach":false,"Tty":false})";
    const Response start_res =
        request("POST", "/exec/" + exec_id + "/start", start_body, json_headers);
    if (start_res.status_code != 200) {
        throw DockerError("exec start " + exec_id + " failed: HTTP " +
                          std::to_string(start_res.status_code) + " " + start_res.body);
    }
    const docker::DemuxedLogs demuxed = docker::demux_all(start_res.body);

    // 3) Inspect the exec for the exit code.
    const Response inspect_res = request("GET", "/exec/" + exec_id + "/json");
    if (inspect_res.status_code != 200) {
        throw DockerError("exec inspect " + exec_id + " failed: HTTP " +
                          std::to_string(inspect_res.status_code) + " " + inspect_res.body);
    }

    ExecResult out;
    out.stdout_data = std::move(demuxed.stdout_data);
    out.stderr_data = std::move(demuxed.stderr_data);
    out.exit_code = docker::parse_exec_exit_code(inspect_res.body);
    return out;
}

void DockerClient::copy_to_container(const std::string& id, const CopyToContainer& source) {
    const std::string tar = docker::build_tar(source);
    // Always extract at the root with relative entry names (build_tar strips the
    // leading '/'), so the target's parent directory must already exist.
    const std::string target =
        "/containers/" + id + "/archive?path=/&noOverwriteDirNonDir=false";
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-tar"}};

    const Response res = request("PUT", target, tar, headers);
    if (res.status_code != 200) {
        throw DockerError("copy_to_container(" + id + ", '" + source.target() + "') failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
}

std::string DockerClient::copy_from_container(const std::string& id,
                                              const std::string& container_path) {
    const std::string target = "/containers/" + id + "/archive?path=" + url_encode(container_path);

    const Response res = request("GET", target);
    if (res.status_code == 200) {
        return res.body;
    }
    if (res.status_code == 404) {
        throw DockerError("copy_from_container(" + id + ", '" + container_path +
                          "') failed: no such container or path: HTTP 404 " + res.body);
    }
    throw DockerError("copy_from_container(" + id + ", '" + container_path + "') failed: HTTP " +
                      std::to_string(res.status_code) + " " + res.body);
}

std::string DockerClient::create_network(
    const std::string& name, const std::vector<std::pair<std::string, std::string>>& labels) {
    nlohmann::json body;
    body["Name"] = name;
    if (!labels.empty()) {
        nlohmann::json json_labels = nlohmann::json::object();
        for (const auto& [key, value] : labels) {
            json_labels[key] = value;
        }
        body["Labels"] = std::move(json_labels);
    }
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    const Response res = request("POST", "/networks/create", body.dump(), headers);
    if (res.status_code != 201) {
        throw DockerError("create_network('" + name + "') failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
    return nlohmann::json::parse(res.body).at("Id").get<std::string>();
}

void DockerClient::remove_network(const std::string& id) {
    const Response res = request("DELETE", "/networks/" + id);
    if (res.status_code != 204) {
        throw DockerError("remove_network(" + id + ") failed: HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
}

} // namespace testcontainers
