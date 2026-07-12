#include "testcontainers/modules/OpenSearch.hpp"

#include <string>
#include <utility>

#include "testcontainers/ConnectionString.hpp"

namespace testcontainers::modules {

OpenSearchImage::OpenSearchImage()
    : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    image_.with_exposed_port(tcp(kPort));
    // A JVM engine unpacking ~1 GB of plugins: warm boots are 10-20s, loaded
    // CI runners have been observed near a minute — 2x that worst is the
    // budget (the MySQL first-boot precedent).
    image_.with_startup_timeout(std::chrono::seconds(120));

    // The managed env is baked HERE, at construction, not at render: every
    // later with_env lands after it in the Env list, and the entrypoint
    // applies the last duplicate — so the user wins without any render-time
    // ordering (the Kafka rule; these are engine tuning, not credential
    // mirrors, so no getter can desync). discovery.type=single-node forms a
    // one-node cluster and bypasses the bootstrap checks (a low host
    // vm.max_map_count only warns); the two security keys keep 9200 on
    // plain HTTP with no credentials; the heap override keeps the server
    // inside small CI runners (the image default is 1g).
    image_.with_env("discovery.type", "single-node");
    image_.with_env("DISABLE_SECURITY_PLUGIN", "true");
    image_.with_env("DISABLE_INSTALL_DEMO_CONFIG", "true");
    image_.with_env("OPENSEARCH_JAVA_OPTS", "-Xms512m -Xmx512m");
}

OpenSearchImage& OpenSearchImage::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

OpenSearchImage& OpenSearchImage::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

OpenSearchImage& OpenSearchImage::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

OpenSearchImage& OpenSearchImage::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

OpenSearchImage& OpenSearchImage::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

OpenSearchImage& OpenSearchImage::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

OpenSearchImage& OpenSearchImage::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

OpenSearchImage& OpenSearchImage::with_wait(WaitFor wait) {
    waits_.push_back(std::move(wait));
    return *this;
}

OpenSearchImage& OpenSearchImage::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

OpenSearchImage& OpenSearchImage::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

OpenSearchImage& OpenSearchImage::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage OpenSearchImage::to_generic() const {
    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config. (The managed env was baked once, in
    // the constructor, so nothing env-shaped is appended here.)
    GenericImage generic = image_;

    if (waits_.empty()) {
        // /_cluster/health, not /: the root endpoint answers from the local
        // node, while cluster health returns 200 only once a cluster manager
        // is elected — what the first index/search call actually needs.
        // Probed through the PUBLISHED port: end-to-end proof of the
        // mapping. Plain HTTP works only because security is disabled.
        generic.with_wait(wait_for::http("/_cluster/health", tcp(kPort)));
    } else {
        for (const WaitFor& wait : waits_) {
            generic.with_wait(wait);
        }
    }

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering, mirroring how with_create_body_patch wins over typed fields.
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

OpenSearchContainer OpenSearchImage::start() const {
    return OpenSearchContainer(to_generic().start());
}

std::string OpenSearchContainer::http_url() const {
    ConnectionString url("http");
    url.with_host(host_).with_port(port_);
    return url.to_string();
}

} // namespace testcontainers::modules
