#include "testcontainers/modules/MinIO.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "../Strings.hpp"
#include "ModuleDetail.hpp"
#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"

namespace testcontainers::modules {

namespace {

/// The bucket list rides in the started hook, invisible to the reuse hash —
/// this label pushes it into the create body. Sorted: the hash must not
/// depend on registration order (`mc mb --ignore-existing` makes the
/// creations order-independent anyway).
constexpr std::string_view kBucketsLabel = "org.testcontainers.minio.buckets";

/// Post-ready bucket creation through the in-image `mc`. The alias is set
/// with the credentials as plain argv — mc's URL forms are a trap here: it
/// signs with the URL text verbatim, so percent-encoded credentials fail
/// authentication and raw special characters break the URL parse. Aliasing
/// also round-trips an authenticated call, so wrong plumbing fails here,
/// loudly, rather than as a later mystery. The alias persists in the
/// container's mc config for follow-up execs.
void create_buckets(DockerClient& client, const std::string& id, const std::string& access_key,
                    const std::string& secret_key, const std::vector<std::string>& buckets) {
    detail::exec_or_throw(client, id,
                          {"mc", "alias", "set", "tc",
                           "http://127.0.0.1:" + std::to_string(MinIOImage::kS3Port), access_key,
                           secret_key},
                          "aliasing the in-image mc to the server");
    for (const std::string& bucket : buckets) {
        detail::exec_or_throw(client, id, {"mc", "mb", "--ignore-existing", "tc/" + bucket},
                              "creating minio bucket '" + bucket + "'");
    }
}

} // namespace

MinIOImage::MinIOImage() : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Baked once. The readiness probe is rendered at to_generic() (so
    // with_wait can replace it); only the published ports live here.
    image_.with_exposed_port(tcp(kS3Port)).with_exposed_port(tcp(kConsolePort));
}

MinIOImage& MinIOImage::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

MinIOImage& MinIOImage::with_access_key(std::string access_key) {
    access_key_ = std::move(access_key);
    return *this;
}

MinIOImage& MinIOImage::with_secret_key(std::string secret_key) {
    secret_key_ = std::move(secret_key);
    return *this;
}

MinIOImage& MinIOImage::with_bucket(std::string name) {
    buckets_.push_back(std::move(name));
    return *this;
}

MinIOImage& MinIOImage::with_env(std::string key, std::string value) {
    image_.with_env(std::move(key), std::move(value));
    return *this;
}

MinIOImage& MinIOImage::with_label(std::string key, std::string value) {
    image_.with_label(std::move(key), std::move(value));
    return *this;
}

MinIOImage& MinIOImage::with_network(std::string network) {
    image_.with_network(std::move(network));
    return *this;
}

MinIOImage& MinIOImage::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

MinIOImage& MinIOImage::with_network_alias(std::string alias) {
    image_.with_network_alias(std::move(alias));
    return *this;
}

MinIOImage& MinIOImage::with_reuse(bool reuse) {
    image_.with_reuse(reuse);
    return *this;
}

MinIOImage& MinIOImage::with_wait(WaitFor wait) {
    waits_.push_back(std::move(wait));
    return *this;
}

MinIOImage& MinIOImage::with_startup_timeout(std::chrono::milliseconds timeout) {
    image_.with_startup_timeout(timeout);
    return *this;
}

MinIOImage& MinIOImage::with_startup_attempts(int n) {
    image_.with_startup_attempts(n);
    return *this;
}

MinIOImage& MinIOImage::with_customizer(std::function<void(GenericImage&)> customize) {
    customizers_.push_back(std::move(customize));
    return *this;
}

GenericImage MinIOImage::to_generic() const {
    // Fail fast, before any daemon contact — the server's own reaction to
    // out-of-policy credentials is a fatal exit ("Invalid credentials")
    // followed by the full startup timeout.
    if (access_key_.size() < 3) {
        throw Error("MinIO access key must be at least 3 characters (with_access_key): the "
                    "server rejects shorter ones at boot");
    }
    if (secret_key_.size() < 8) {
        throw Error("MinIO secret key must be at least 8 characters (with_secret_key): the "
                    "server rejects shorter ones at boot (also why this module cannot default "
                    "to the house \"test\")");
    }
    for (const std::string& bucket : buckets_) {
        if (bucket.empty()) {
            throw Error("MinIO bucket name must not be empty (with_bucket)");
        }
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // The module owns the command: without a fixed --console-address the
    // server picks a random console port every boot, which could not be
    // exposed.
    generic.with_cmd({"server", "/data", "--console-address", ":" + std::to_string(kConsolePort)});

    // The credential pair is appended AFTER any pass-through env (the image's
    // shell entrypoint applies the last duplicate in the Env list), so the
    // getters and the bucket hook can never desynchronize from the server.
    generic.with_env("MINIO_ROOT_USER", access_key_);
    generic.with_env("MINIO_ROOT_PASSWORD", secret_key_);

    if (waits_.empty()) {
        // /minio/health/cluster is the one health endpoint gated on the
        // object layer having write quorum — on a single node, on the store
        // being initialized and writable (503 before). /live and /ready
        // answer 200 as soon as the process serves HTTP. Probed through the
        // PUBLISHED port: end-to-end proof of the mapping.
        generic.with_wait(wait_for::http("/minio/health/cluster", tcp(kS3Port)));
    } else {
        for (const WaitFor& wait : waits_) {
            generic.with_wait(wait);
        }
    }

    if (!buckets_.empty()) {
        std::vector<std::string> sorted = buckets_;
        std::sort(sorted.begin(), sorted.end());
        generic.with_label(std::string(kBucketsLabel), testcontainers::detail::join(sorted, ","));

        const std::string access_key = access_key_;
        const std::string secret_key = secret_key_;
        const std::vector<std::string> buckets = buckets_;
        generic.with_started_hook(
            [access_key, secret_key, buckets](DockerClient& client, const std::string& id) {
                create_buckets(client, id, access_key, secret_key, buckets);
            });
    }

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering, mirroring how with_create_body_patch wins over typed fields.
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

MinIOContainer MinIOImage::start() const {
    return MinIOContainer(to_generic().start(), access_key_, secret_key_);
}

std::string MinIOContainer::s3_url() const {
    ConnectionString url("http");
    url.with_host(host_).with_port(s3_port_);
    return url.to_string();
}

std::string MinIOContainer::console_url() const {
    ConnectionString url("http");
    url.with_host(host_).with_port(console_port_);
    return url.to_string();
}

} // namespace testcontainers::modules
