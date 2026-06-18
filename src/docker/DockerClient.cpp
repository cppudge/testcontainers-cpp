#include "testcontainers/docker/DockerClient.hpp"

#include "docker/ApiMapping.hpp"
#include "docker/Transport.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/version.hpp"

#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>

#include <cctype>
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
    http::response<http::string_body> res;
    http::read(stream, buffer, res, ec);
    if (ec && ec != http::error::end_of_stream) {
        throw DockerError("Failed to read response from Docker (" + std::string(method) + " " +
                          std::string(target) + "): " + ec.message());
    }

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

void DockerClient::pull_image(const std::string& image) {
    const auto [name, tag] = docker::split_image(image);
    const std::string target =
        "/images/create?fromImage=" + url_encode(name) + "&tag=" + url_encode(tag);

    const Response res = request("POST", target);
    if (res.status_code != 200) {
        throw DockerError("Failed to pull image '" + image + "': HTTP " +
                          std::to_string(res.status_code) + " " + res.body);
    }
    // Docker streams progress as newline-delimited JSON and returns 200 even on
    // failure, embedding the error in the stream.
    docker::throw_if_pull_error(res.body, image);
}

std::string DockerClient::create_container(const CreateContainerSpec& spec) {
    const std::string body = docker::build_create_body(spec).dump();
    std::string target = "/containers/create";
    if (spec.name) {
        target += "?name=" + url_encode(*spec.name);
    }
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}};

    Response res = request("POST", target, body, headers);
    if (res.status_code == 404) {
        // Image not present locally — pull it and retry once.
        pull_image(spec.image);
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

} // namespace testcontainers
