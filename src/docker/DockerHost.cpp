#define _CRT_SECURE_NO_WARNINGS // std::getenv on MSVC

#include "testcontainers/docker/DockerHost.hpp"

#include "testcontainers/Error.hpp"

#include <cctype>
#include <cstdlib>

namespace testcontainers {

namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Parse "host:port" (optionally followed by a path) into the host fields.
void parse_host_port(std::string rest, std::string& hostname, std::uint16_t& port,
                     std::uint16_t default_port) {
    if (const auto slash = rest.find('/'); slash != std::string::npos) {
        rest = rest.substr(0, slash); // drop any trailing path
    }

    std::string host;
    std::string port_str;
    if (!rest.empty() && rest.front() == '[') {
        // IPv6 literal: [::1]:2375
        const auto close = rest.find(']');
        host = rest.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        if (close != std::string::npos && close + 1 < rest.size() && rest[close + 1] == ':') {
            port_str = rest.substr(close + 2);
        }
    } else if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
        host = rest.substr(0, colon);
        port_str = rest.substr(colon + 1);
    } else {
        host = rest;
    }

    hostname = host;
    port = default_port;
    if (!port_str.empty()) {
        try {
            port = static_cast<std::uint16_t>(std::stoi(port_str));
        } catch (const std::exception&) {
            throw DockerError("Invalid port in Docker host: " + port_str);
        }
    }
}

} // namespace

DockerHost DockerHost::parse(const std::string& url) {
    DockerHost h;
    h.url_ = url;

    std::string scheme;
    std::string rest;
    if (const auto sep = url.find("://"); sep != std::string::npos) {
        scheme = to_lower(url.substr(0, sep));
        rest = url.substr(sep + 3);
    } else if (!url.empty() && (url.front() == '/' || url.front() == '\\')) {
        scheme = "unix"; // bare path
        rest = url;
    } else {
        scheme = "tcp"; // bare host:port
        rest = url;
    }

    if (scheme == "unix") {
        h.scheme_ = DockerScheme::Unix;
        h.path_ = rest;
    } else if (scheme == "npipe") {
        h.scheme_ = DockerScheme::NamedPipe;
        h.path_ = rest;
    } else if (scheme == "https") {
        h.scheme_ = DockerScheme::Https;
        parse_host_port(rest, h.hostname_, h.port_, 2376);
    } else if (scheme == "tcp" || scheme == "http") {
        h.scheme_ = DockerScheme::Tcp;
        parse_host_port(rest, h.hostname_, h.port_, 2375);
    } else {
        throw DockerError("Unsupported Docker host scheme: '" + scheme + "' in " + url);
    }

    return h;
}

DockerHost DockerHost::resolve() {
    if (const char* docker_host = std::getenv("DOCKER_HOST"); docker_host && *docker_host) {
        return parse(docker_host);
    }
#if defined(_WIN32)
    return parse("npipe:////./pipe/docker_engine");
#else
    return parse("unix:///var/run/docker.sock");
#endif
}

std::string DockerHost::http_host() const {
    switch (scheme_) {
    case DockerScheme::Unix:
    case DockerScheme::NamedPipe:
        return "localhost";
    case DockerScheme::Tcp:
    case DockerScheme::Https:
        return hostname_.empty() ? "localhost" : hostname_;
    }
    return "localhost";
}

} // namespace testcontainers
