#include "testcontainers/modules/RustFS.hpp"

#include <string>
#include <utility>

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"

namespace testcontainers::modules {

RustFSImage::RustFSImage() : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Baked once. The readiness probe is rendered at to_generic() (so
    // with_wait can replace it); only the published ports live here. No
    // command: the image's entrypoint launches the server against the
    // baked-in RUSTFS_VOLUMES=/data on its own.
    image_.with_exposed_port(tcp(kS3Port)).with_exposed_port(tcp(kConsolePort));
}

RustFSImage& RustFSImage::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

RustFSImage& RustFSImage::with_access_key(std::string access_key) {
    access_key_ = std::move(access_key);
    return *this;
}

RustFSImage& RustFSImage::with_secret_key(std::string secret_key) {
    secret_key_ = std::move(secret_key);
    return *this;
}

RustFSImage& RustFSImage::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

RustFSImage& RustFSImage::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

RustFSImage& RustFSImage::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

RustFSImage& RustFSImage::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

RustFSImage& RustFSImage::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

RustFSImage& RustFSImage::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

RustFSImage& RustFSImage::with_wait(WaitFor wait) {
    waits_.push_back(std::move(wait));
    return *this;
}

RustFSImage& RustFSImage::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

RustFSImage& RustFSImage::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

RustFSImage& RustFSImage::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage RustFSImage::to_generic() const {
    // Fail fast, before any daemon contact. Only emptiness is checked: the
    // server itself imposes no documented credential length rules (verified
    // against the pinned image — one-character keys boot and authenticate).
    if (access_key_.empty()) {
        throw Error("RustFS access key must not be empty (with_access_key)");
    }
    if (secret_key_.empty()) {
        throw Error("RustFS secret key must not be empty (with_secret_key)");
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // The credential pair is appended AFTER any pass-through env (the image's
    // shell entrypoint applies the last duplicate in the Env list), so the
    // getters can never desynchronize from the server.
    generic.with_env("RUSTFS_ACCESS_KEY", access_key_);
    generic.with_env("RUSTFS_SECRET_KEY", secret_key_);

    if (waits_.empty()) {
        // The server's own health endpoint (the project's compose healthcheck
        // curls the same path); it answers 200 once the storage/IAM/lock
        // subsystems report ready. Probed through the PUBLISHED port:
        // end-to-end proof of the mapping. Chosen over a log-message wait —
        // an answering HTTP stack proves more than a banner, and log text is
        // the least stable API a beta has.
        generic.with_wait(wait_for::http("/health", tcp(kS3Port)));
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

RustFSContainer RustFSImage::start() const {
    return RustFSContainer(to_generic().start(), access_key_, secret_key_);
}

std::string RustFSContainer::s3_url() const {
    ConnectionString url("http");
    url.with_host(host_).with_port(s3_port_);
    return url.to_string();
}

std::string RustFSContainer::console_url() const {
    // The UI is served under this prefix; the console port answers 403 at
    // the bare root, which reads like a failure to a human pasting the URL.
    ConnectionString url("http");
    url.with_host(host_).with_port(console_port_);
    return url.to_string() + "/rustfs/console/";
}

} // namespace testcontainers::modules
