#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/Ulimit.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers {

/// When to pull the image before creating the container.
enum class ImagePullPolicy {
    Default, ///< pull only if missing locally (lazy, on create 404) — today's behavior
    Always,  ///< always pull before create (even if present locally)
};

/// A reusable, copyable description of a container to run: image reference,
/// exposed ports, environment, command, labels, and readiness conditions.
///
/// The `with_*` builders mutate in place and return `*this` (ref-qualified), so
/// a named config can be configured incrementally and started many times — no
/// consume-self, no use-after-move.
class GenericImage {
public:
    /// Construct from an image name and tag (tag defaults to "latest").
    explicit GenericImage(std::string image, std::string tag = "latest")
        : image_(std::move(image)), tag_(std::move(tag)) {}

    /// Construct from a full image reference "name[:tag]" (tag defaults to "latest").
    /// Pairs with ImageFromDockerfile::build(): GenericImage::from_reference(img.build()).
    static GenericImage from_reference(const std::string& reference);

    // --- In-place, ref-qualified builders ---

    GenericImage& with_exposed_port(ContainerPort p) & {
        exposed_ports_.push_back(p);
        return *this;
    }
    GenericImage&& with_exposed_port(ContainerPort p) && {
        exposed_ports_.push_back(p);
        return std::move(*this);
    }

    GenericImage& with_env(std::string key, std::string value) & {
        env_.emplace_back(std::move(key), std::move(value));
        return *this;
    }
    GenericImage&& with_env(std::string key, std::string value) && {
        env_.emplace_back(std::move(key), std::move(value));
        return std::move(*this);
    }

    GenericImage& with_cmd(std::vector<std::string> cmd) & {
        cmd_ = std::move(cmd);
        return *this;
    }
    GenericImage&& with_cmd(std::vector<std::string> cmd) && {
        cmd_ = std::move(cmd);
        return std::move(*this);
    }

    GenericImage& with_entrypoint(std::vector<std::string> entrypoint) & {
        entrypoint_ = std::move(entrypoint);
        return *this;
    }
    GenericImage&& with_entrypoint(std::vector<std::string> entrypoint) && {
        entrypoint_ = std::move(entrypoint);
        return std::move(*this);
    }

    GenericImage& with_working_dir(std::string working_dir) & {
        working_dir_ = std::move(working_dir);
        return *this;
    }
    GenericImage&& with_working_dir(std::string working_dir) && {
        working_dir_ = std::move(working_dir);
        return std::move(*this);
    }

    GenericImage& with_user(std::string user) & {
        user_ = std::move(user);
        return *this;
    }
    GenericImage&& with_user(std::string user) && {
        user_ = std::move(user);
        return std::move(*this);
    }

    GenericImage& with_privileged(bool privileged = true) & {
        privileged_ = privileged;
        return *this;
    }
    GenericImage&& with_privileged(bool privileged = true) && {
        privileged_ = privileged;
        return std::move(*this);
    }

    GenericImage& with_mount(Mount mount) & {
        mounts_.push_back(std::move(mount));
        return *this;
    }
    GenericImage&& with_mount(Mount mount) && {
        mounts_.push_back(std::move(mount));
        return std::move(*this);
    }

    /// Copy a host file or in-memory bytes into the container after it is
    /// created and before it is started (the target's parent directory must
    /// already exist in the image). Add several to copy multiple entries.
    GenericImage& with_copy_to(CopyToContainer source) & {
        copy_to_sources_.push_back(std::move(source));
        return *this;
    }
    GenericImage&& with_copy_to(CopyToContainer source) && {
        copy_to_sources_.push_back(std::move(source));
        return std::move(*this);
    }

    GenericImage& with_label(std::string key, std::string value) & {
        labels_.emplace_back(std::move(key), std::move(value));
        return *this;
    }
    GenericImage&& with_label(std::string key, std::string value) && {
        labels_.emplace_back(std::move(key), std::move(value));
        return std::move(*this);
    }

    GenericImage& with_wait(WaitFor w) & {
        waits_.push_back(std::move(w));
        return *this;
    }
    GenericImage&& with_wait(WaitFor w) && {
        waits_.push_back(std::move(w));
        return std::move(*this);
    }

    GenericImage& with_startup_timeout(std::chrono::milliseconds timeout) & {
        startup_timeout_ = timeout;
        return *this;
    }
    GenericImage&& with_startup_timeout(std::chrono::milliseconds timeout) && {
        startup_timeout_ = timeout;
        return std::move(*this);
    }

    GenericImage& with_healthcheck(Healthcheck hc) & {
        healthcheck_ = std::move(hc);
        return *this;
    }
    GenericImage&& with_healthcheck(Healthcheck hc) && {
        healthcheck_ = std::move(hc);
        return std::move(*this);
    }

    /// Join the container to a user-defined network (`HostConfig.NetworkMode`).
    /// Containers on the same network resolve each other by container name.
    GenericImage& with_network(std::string network) & {
        network_ = std::move(network);
        return *this;
    }
    GenericImage&& with_network(std::string network) && {
        network_ = std::move(network);
        return std::move(*this);
    }

    /// Add a DNS alias for this container on its network (`NetworkingConfig`).
    /// Peers on the same network can resolve this container by the alias in
    /// addition to its container name. Requires `with_network(...)` to take
    /// effect; aliases without a target network are ignored.
    GenericImage& with_network_alias(std::string alias) & {
        network_aliases_.push_back(std::move(alias));
        return *this;
    }
    GenericImage&& with_network_alias(std::string alias) && {
        network_aliases_.push_back(std::move(alias));
        return std::move(*this);
    }

    /// Set an explicit container name (passed as `?name=` on create). Useful so
    /// peers on the same network can resolve this container by name.
    GenericImage& with_container_name(std::string name) & {
        container_name_ = std::move(name);
        return *this;
    }
    GenericImage&& with_container_name(std::string name) && {
        container_name_ = std::move(name);
        return std::move(*this);
    }

    /// Pin the create platform as a free-form "<os>/<arch>" string (e.g.
    /// "windows/amd64"), sent as the `?platform=` query on create. Useful to
    /// select a Windows image variant on a Windows-containers engine.
    GenericImage& with_platform(std::string platform) & {
        platform_ = std::move(platform);
        return *this;
    }
    GenericImage&& with_platform(std::string platform) && {
        platform_ = std::move(platform);
        return std::move(*this);
    }

    /// Supply explicit registry credentials for pulling a private image. When
    /// unset, credentials are auto-resolved from the Docker config (if any).
    GenericImage& with_registry_auth(RegistryAuth auth) & {
        registry_auth_ = std::move(auth);
        return *this;
    }
    GenericImage&& with_registry_auth(RegistryAuth auth) && {
        registry_auth_ = std::move(auth);
        return std::move(*this);
    }

    /// Set a hard memory limit in bytes (`HostConfig.Memory`).
    GenericImage& with_memory_limit(std::int64_t bytes) & {
        memory_bytes_ = bytes;
        return *this;
    }
    GenericImage&& with_memory_limit(std::int64_t bytes) && {
        memory_bytes_ = bytes;
        return std::move(*this);
    }

    /// Set the size of `/dev/shm` in bytes (`HostConfig.ShmSize`).
    GenericImage& with_shm_size(std::int64_t bytes) & {
        shm_size_bytes_ = bytes;
        return *this;
    }
    GenericImage&& with_shm_size(std::int64_t bytes) && {
        shm_size_bytes_ = bytes;
        return std::move(*this);
    }

    /// Add a process resource limit (`HostConfig.Ulimits`), e.g.
    /// `with_ulimit("nofile", 1024, 2048)`. Add several to set multiple limits.
    GenericImage& with_ulimit(std::string name, std::int64_t soft, std::int64_t hard) & {
        ulimits_.push_back(Ulimit{std::move(name), soft, hard});
        return *this;
    }
    GenericImage&& with_ulimit(std::string name, std::int64_t soft, std::int64_t hard) && {
        ulimits_.push_back(Ulimit{std::move(name), soft, hard});
        return std::move(*this);
    }

    /// Add a Linux capability to grant (`HostConfig.CapAdd`), e.g. "NET_ADMIN".
    GenericImage& with_cap_add(std::string cap) & {
        cap_add_.push_back(std::move(cap));
        return *this;
    }
    GenericImage&& with_cap_add(std::string cap) && {
        cap_add_.push_back(std::move(cap));
        return std::move(*this);
    }

    /// Add a Linux capability to drop (`HostConfig.CapDrop`).
    GenericImage& with_cap_drop(std::string cap) & {
        cap_drop_.push_back(std::move(cap));
        return *this;
    }
    GenericImage&& with_cap_drop(std::string cap) && {
        cap_drop_.push_back(std::move(cap));
        return std::move(*this);
    }

    /// Add an `/etc/hosts` entry (`HostConfig.ExtraHosts`) mapping `host` to `ip`.
    GenericImage& with_extra_host(std::string host, std::string ip) & {
        extra_hosts_.push_back(host + ":" + ip);
        return *this;
    }
    GenericImage&& with_extra_host(std::string host, std::string ip) && {
        extra_hosts_.push_back(host + ":" + ip);
        return std::move(*this);
    }

    /// Deep-merge a raw Docker `/containers/create` body fragment into the create
    /// body (RFC 7386 merge applied AFTER our typed fields, so it overrides them).
    /// This is the escape hatch for any field not exposed as a typed setter: nest
    /// HostConfig fields under `"HostConfig"`. `json_object` must be a JSON object.
    GenericImage& with_create_body_patch(std::string json_object) & {
        create_body_patch_ = std::move(json_object);
        return *this;
    }
    GenericImage&& with_create_body_patch(std::string json_object) && {
        create_body_patch_ = std::move(json_object);
        return std::move(*this);
    }

    /// Control whether the image is pulled before create. `Default` keeps the
    /// lazy behavior (pull only on a create 404); `Always` pulls before create
    /// even when the image is already present locally.
    GenericImage& with_image_pull_policy(ImagePullPolicy policy) & {
        pull_policy_ = policy;
        return *this;
    }
    GenericImage&& with_image_pull_policy(ImagePullPolicy policy) && {
        pull_policy_ = policy;
        return std::move(*this);
    }

    /// Enable container reuse (à la testcontainers `.withReuse(true)`). When
    /// reuse is also enabled globally (`testcontainers.reuse.enable=true` in
    /// ~/.testcontainers.properties or `TESTCONTAINERS_REUSE_ENABLE=true`),
    /// `start()` first looks for an already-running container matching this
    /// config (by a stable reuse-hash label) and ADOPTS it instead of creating a
    /// new one; the returned handle is persistent (it does NOT remove the
    /// container on destruction, and the container is NOT Ryuk-reaped, so it
    /// survives across runs). When reuse is not enabled globally this is a no-op:
    /// `start()` behaves exactly like a normal (reaped, auto-removed) container.
    GenericImage& with_reuse(bool reuse = true) & {
        reuse_ = reuse;
        return *this;
    }
    GenericImage&& with_reuse(bool reuse = true) && {
        reuse_ = reuse;
        return std::move(*this);
    }

    /// Override how the image reference is rewritten before create. When set this
    /// REPLACES the default env-prefix substitution (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX);
    /// the function receives "name:tag" and returns the reference to actually use.
    GenericImage& with_image_name_substitutor(
        std::function<std::string(const std::string&)> fn) & {
        substitutor_ = std::move(fn);
        return *this;
    }
    GenericImage&& with_image_name_substitutor(
        std::function<std::string(const std::string&)> fn) && {
        substitutor_ = std::move(fn);
        return std::move(*this);
    }

    // --- Getters ---

    const std::string& image() const noexcept { return image_; }
    const std::string& tag() const noexcept { return tag_; }
    const std::vector<ContainerPort>& exposed_ports() const noexcept { return exposed_ports_; }
    const std::vector<std::pair<std::string, std::string>>& env() const noexcept { return env_; }
    const std::vector<std::string>& cmd() const noexcept { return cmd_; }
    const std::vector<std::string>& entrypoint() const noexcept { return entrypoint_; }
    const std::optional<std::string>& working_dir() const noexcept { return working_dir_; }
    const std::optional<std::string>& user() const noexcept { return user_; }
    bool privileged() const noexcept { return privileged_; }
    const std::vector<Mount>& mounts() const noexcept { return mounts_; }
    const std::vector<CopyToContainer>& copy_to_sources() const noexcept {
        return copy_to_sources_;
    }
    const std::vector<std::pair<std::string, std::string>>& labels() const noexcept {
        return labels_;
    }
    const std::vector<WaitFor>& waits() const noexcept { return waits_; }
    std::chrono::milliseconds startup_timeout() const noexcept { return startup_timeout_; }
    const std::optional<Healthcheck>& healthcheck() const noexcept { return healthcheck_; }
    const std::optional<std::string>& network() const noexcept { return network_; }
    const std::vector<std::string>& network_aliases() const noexcept { return network_aliases_; }
    const std::optional<std::string>& container_name() const noexcept { return container_name_; }
    const std::optional<std::string>& platform() const noexcept { return platform_; }
    const std::optional<RegistryAuth>& registry_auth() const noexcept { return registry_auth_; }
    const std::optional<std::int64_t>& memory_limit() const noexcept { return memory_bytes_; }
    const std::optional<std::int64_t>& shm_size() const noexcept { return shm_size_bytes_; }
    const std::vector<Ulimit>& ulimits() const noexcept { return ulimits_; }
    const std::vector<std::string>& cap_add() const noexcept { return cap_add_; }
    const std::vector<std::string>& cap_drop() const noexcept { return cap_drop_; }
    const std::vector<std::string>& extra_hosts() const noexcept { return extra_hosts_; }
    const std::string& create_body_patch() const noexcept { return create_body_patch_; }
    ImagePullPolicy image_pull_policy() const noexcept { return pull_policy_; }
    bool reuse() const noexcept { return reuse_; }
    const std::function<std::string(const std::string&)>& image_name_substitutor() const noexcept {
        return substitutor_;
    }

    /// Create, start, and wait for a container from this image, returning a RAII
    /// handle that removes the container on destruction. Throws on failure
    /// (best-effort removing a container that started but never became ready).
    Container start() const;

private:
    std::string image_;
    std::string tag_;
    std::vector<ContainerPort> exposed_ports_;
    std::vector<std::pair<std::string, std::string>> env_;
    std::vector<std::string> cmd_;
    std::vector<std::string> entrypoint_;
    std::optional<std::string> working_dir_;
    std::optional<std::string> user_;
    bool privileged_ = false;
    std::vector<Mount> mounts_;
    std::vector<CopyToContainer> copy_to_sources_;
    std::vector<std::pair<std::string, std::string>> labels_;
    std::vector<WaitFor> waits_;
    std::chrono::milliseconds startup_timeout_{std::chrono::seconds(60)};
    std::optional<Healthcheck> healthcheck_;
    std::optional<std::string> network_;
    std::vector<std::string> network_aliases_;
    std::optional<std::string> container_name_;
    std::optional<std::string> platform_;
    std::optional<RegistryAuth> registry_auth_;
    std::optional<std::int64_t> memory_bytes_;
    std::optional<std::int64_t> shm_size_bytes_;
    std::vector<Ulimit> ulimits_;
    std::vector<std::string> cap_add_;
    std::vector<std::string> cap_drop_;
    std::vector<std::string> extra_hosts_;
    std::string create_body_patch_;
    ImagePullPolicy pull_policy_ = ImagePullPolicy::Default;
    std::function<std::string(const std::string&)> substitutor_;
    bool reuse_ = false;
};

} // namespace testcontainers
