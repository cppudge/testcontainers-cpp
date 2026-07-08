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
#include "testcontainers/ContainerRequest.hpp"
#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Lifecycle.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/RegistryAuth.hpp"
#include "testcontainers/Ulimit.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"

namespace testcontainers {

/// A reusable, copyable description of a container to run: image reference,
/// exposed ports, environment, command, labels, and readiness conditions.
///
/// The `with_*` builders mutate in place and return `*this` by reference, so a
/// named config can be configured incrementally and started many times — no
/// consume-self, no use-after-move. A single unqualified overload chains on both
/// a named lvalue and a temporary (`GenericImage("redis","7.2").with_*()...`).
///
/// The "create" fields (command, mounts, labels, host-config knobs, …) are held
/// directly in an embedded `CreateContainerSpec spec_`, so a new create option is
/// added in one place (a setter + getter forwarding to `spec_`) without a
/// parallel private field and a hand-written copy in `start()`. Only the fields
/// that need real translation before create — the image reference (resolve +
/// substitution), env (`pair` → "KEY=VALUE"), and exposed ports (`ContainerPort`
/// → "6379/tcp" + publish-all) — are kept as separate domain fields, alongside
/// the orchestration-only state (waits, copy-to sources, hooks, reuse, …).
class GenericImage {
public:
    /// Construct from an image name and tag.
    explicit GenericImage(std::string image, std::string tag = "latest")
        : image_(std::move(image)), tag_(std::move(tag)) {}

    /// Construct from a full image reference "name[:tag]" (tag defaults to "latest"),
    /// e.g. for an externally-resolved reference. To run an image built by
    /// GenericBuildableImage, prefer its `build()`, which returns a GenericImage.
    static GenericImage from_reference(const std::string& reference);

    /// True when the image `<name>:<tag>` is already present on the daemon —
    /// name and tag are separate, exactly like the constructor. A purely local
    /// check (`GET /images/{ref}/json`), no registry is contacted. Useful to
    /// skip an expensive `GenericBuildableImage::build()` when a previous run
    /// already produced the image:
    /// ```cpp
    /// GenericImage img = GenericImage::exists("app", "v1")
    ///                        ? GenericImage("app", "v1")
    ///                        : app_buildable.build();
    /// ```
    /// `name` must not embed a tag: for a full "name[:tag]" reference (or a
    /// digest), call `DockerClient::image_exists` directly.
    /// Note: presence says nothing about freshness — if the build inputs can
    /// change under the same tag, derive the tag from a hash of the inputs.
    /// Throws DockerError when the daemon cannot be reached or errors.
    static bool exists(const std::string& name, const std::string& tag = "latest");

    /// A structured snapshot of the image `<name>:<tag>` on the daemon
    /// (`GET /images/{ref}/json`): id, repo tags/digests, creation time, os /
    /// architecture, size, and the image config (labels, env, cmd, entrypoint,
    /// exposed ports, workdir, user). A purely local lookup, no registry is
    /// contacted; name and tag are separate, exactly like `exists`. Throws
    /// DockerError if the image is absent (NotFoundError) or the daemon
    /// cannot be reached. For a full "name[:tag]" reference, an image ID, or a
    /// digest, call `DockerClient::inspect_image` directly.
    static ImageInspect inspect(const std::string& name, const std::string& tag = "latest");

    /// `inspect(image(), tag())` — a structured snapshot of THIS config's image
    /// on the daemon. The reference is used verbatim: no image-name substitutor
    /// or env-prefix rewrite is applied (exactly like `exists`; `start()` is
    /// where substitution happens).
    ImageInspect inspect() const;

    // --- In-place builders (single overload; chains on lvalues and temporaries) ---

    GenericImage& with_exposed_port(ContainerPort p) {
        exposed_ports_.push_back(p);
        return *this;
    }

    GenericImage& with_env(std::string key, std::string value) {
        env_.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    GenericImage& with_cmd(std::vector<std::string> cmd) {
        spec_.cmd = std::move(cmd);
        return *this;
    }

    GenericImage& with_entrypoint(std::vector<std::string> entrypoint) {
        spec_.entrypoint = std::move(entrypoint);
        return *this;
    }

    GenericImage& with_working_dir(std::string working_dir) {
        spec_.working_dir = std::move(working_dir);
        return *this;
    }

    GenericImage& with_user(std::string user) {
        spec_.user = std::move(user);
        return *this;
    }

    GenericImage& with_privileged(bool privileged = true) {
        spec_.privileged = privileged;
        return *this;
    }

    /// Container isolation technology (`HostConfig.Isolation`) — Windows
    /// daemons only: "process" or "hyperv". Docker Desktop defaults Windows
    /// containers to Hyper-V isolation, under which some operations are
    /// unavailable (e.g. copying files into a RUNNING container); "process"
    /// lifts that but requires the image build to match the host build.
    /// Leave unset on Linux daemons — they REJECT any non-default value at
    /// create time ("Invalid isolation: ... Unix only supports 'default'").
    GenericImage& with_isolation(std::string isolation) {
        spec_.isolation = std::move(isolation);
        return *this;
    }

    /// Allocate a pseudo-TTY for the container (`Tty=true`). The log stream is
    /// then raw/unframed (no multiplex header) with no separate stderr channel —
    /// `Container::logs()` / `follow_logs()` read it without demuxing. Note a TTY
    /// also rewrites `\n` to `\r\n`.
    GenericImage& with_tty(bool tty = true) {
        spec_.tty = tty;
        return *this;
    }

    GenericImage& with_mount(Mount mount) {
        spec_.mounts.push_back(std::move(mount));
        return *this;
    }

    /// Copy a host file, in-memory bytes, or a host directory tree into the
    /// container after it is created and before it is started. For a
    /// single-file source the target's parent directory must already exist in
    /// the image; a directory source creates the target chain itself. Add
    /// several to copy multiple entries.
    GenericImage& with_copy_to(CopyToContainer source) {
        copy_to_sources_.push_back(std::move(source));
        return *this;
    }

    GenericImage& with_label(std::string key, std::string value) {
        spec_.labels.emplace_back(std::move(key), std::move(value));
        return *this;
    }

    GenericImage& with_wait(WaitFor w) {
        waits_.push_back(std::move(w));
        return *this;
    }

    GenericImage& with_startup_timeout(std::chrono::milliseconds timeout) {
        startup_timeout_ = timeout;
        return *this;
    }

    GenericImage& with_healthcheck(Healthcheck hc) {
        spec_.healthcheck = std::move(hc);
        return *this;
    }

    /// Join the container to a user-defined network (`HostConfig.NetworkMode`).
    /// Containers on the same network resolve each other by container name.
    GenericImage& with_network(std::string network) {
        spec_.network = std::move(network);
        return *this;
    }

    /// Add a DNS alias for this container on its network (`NetworkingConfig`).
    /// Peers on the same network can resolve this container by the alias in
    /// addition to its container name. Requires `with_network(...)` to take
    /// effect; aliases without a target network are ignored.
    GenericImage& with_network_alias(std::string alias) {
        spec_.network_aliases.push_back(std::move(alias));
        return *this;
    }

    /// Assign a fixed IPv4 address to this container on its network
    /// (`NetworkingConfig` endpoint `IPAMConfig.IPv4Address`). Requires
    /// `with_network(...)` on a user-defined network whose subnet contains the
    /// address (e.g. one created via `Network::builder().with_subnet(...)`);
    /// without a target network there is no endpoint to pin, so this is ignored.
    GenericImage& with_static_ipv4(std::string ip) {
        spec_.static_ipv4 = std::move(ip);
        return *this;
    }

    /// Set an explicit container name (passed as `?name=` on create). Useful so
    /// peers on the same network can resolve this container by name.
    GenericImage& with_container_name(std::string name) {
        spec_.name = std::move(name);
        return *this;
    }

    /// Pin the create platform as a free-form "<os>/<arch>" string (e.g.
    /// "windows/amd64"), sent as the `?platform=` query on create. Useful to
    /// select a Windows image variant on a Windows-containers engine.
    GenericImage& with_platform(std::string platform) {
        spec_.platform = std::move(platform);
        return *this;
    }

    /// Supply explicit registry credentials for pulling a private image. When
    /// unset, credentials are auto-resolved from the Docker config (if any).
    GenericImage& with_registry_auth(RegistryAuth auth) {
        registry_auth_ = std::move(auth);
        return *this;
    }

    /// Set a hard memory limit in bytes (`HostConfig.Memory`).
    GenericImage& with_memory_limit(std::int64_t bytes) {
        spec_.memory_bytes = bytes;
        return *this;
    }

    /// Set the size of `/dev/shm` in bytes (`HostConfig.ShmSize`).
    GenericImage& with_shm_size(std::int64_t bytes) {
        spec_.shm_size_bytes = bytes;
        return *this;
    }

    /// Add a process resource limit (`HostConfig.Ulimits`), e.g.
    /// `with_ulimit("nofile", 1024, 2048)`. Add several to set multiple limits.
    GenericImage& with_ulimit(std::string name, std::int64_t soft, std::int64_t hard) {
        spec_.ulimits.push_back(Ulimit{std::move(name), soft, hard});
        return *this;
    }

    /// Add a Linux capability to grant (`HostConfig.CapAdd`), e.g. "NET_ADMIN".
    GenericImage& with_cap_add(std::string cap) {
        spec_.cap_add.push_back(std::move(cap));
        return *this;
    }

    /// Add a Linux capability to drop (`HostConfig.CapDrop`).
    GenericImage& with_cap_drop(std::string cap) {
        spec_.cap_drop.push_back(std::move(cap));
        return *this;
    }

    /// Add an `/etc/hosts` entry (`HostConfig.ExtraHosts`) mapping `host` to `ip`.
    GenericImage& with_extra_host(const std::string& host, const std::string& ip) {
        spec_.extra_hosts.push_back(host + ":" + ip);
        return *this;
    }

    /// Expose a port of the HOST (the machine the tests run on) to this
    /// container: services listening on `127.0.0.1:<port>` in the test process
    /// become reachable from inside the container at
    /// `host.testcontainers.internal:<port>`. Add several to expose multiple
    /// ports.
    ///
    /// Works regardless of where the daemon runs (Docker Desktop VM, remote
    /// engine): the first start() needing it launches one process-wide
    /// `testcontainers/sshd` sidecar container and tunnels each exposed port
    /// back to the host through an SSH remote forward. Requires a
    /// Linux-containers daemon; supported on the default bridge network and
    /// user-defined networks (`with_network`) — not on network modes "host",
    /// "none", or "container:...". If the library was built with
    /// `TC_HOST_PORT_FORWARDING=OFF` (a build without libssh2/OpenSSL),
    /// start() throws a DockerError naming that option.
    GenericImage& with_exposed_host_port(std::uint16_t port) {
        host_access_ports_.push_back(port);
        return *this;
    }

    /// Deep-merge a raw Docker `/containers/create` body fragment into the create
    /// body (RFC 7386 merge applied AFTER our typed fields, so it overrides them).
    /// This is the escape hatch for any field not exposed as a typed setter: nest
    /// HostConfig fields under `"HostConfig"`. `json_object` must be a JSON object.
    GenericImage& with_create_body_patch(std::string json_object) {
        spec_.create_body_patch = std::move(json_object);
        return *this;
    }

    /// Control whether the image is pulled before create. `Default` keeps the
    /// lazy behavior (pull only on a create 404); `Always` pulls before create
    /// even when the image is already present locally.
    GenericImage& with_image_pull_policy(ImagePullPolicy policy) {
        pull_policy_ = policy;
        return *this;
    }

    /// Enable container reuse. When reuse is also enabled globally (`testcontainers.reuse.enable=true` in
    /// ~/.testcontainers.properties or `TESTCONTAINERS_REUSE_ENABLE=true`),
    /// `start()` first looks for an already-running container matching this
    /// config (by a stable reuse-hash label) and ADOPTS it instead of creating a
    /// new one; the returned handle is persistent (it does NOT remove the
    /// container on destruction, and the container is NOT Ryuk-reaped, so it
    /// survives across runs). When reuse is not enabled globally this is a no-op:
    /// `start()` behaves exactly like a normal (reaped, auto-removed) container.
    GenericImage& with_reuse(bool reuse = true) {
        reuse_ = reuse;
        return *this;
    }

    /// Override how the image reference is rewritten before create. When set this
    /// REPLACES the default env-prefix substitution (TESTCONTAINERS_HUB_IMAGE_NAME_PREFIX);
    /// the function receives "name:tag" and returns the reference to actually use.
    GenericImage& with_image_name_substitutor(std::function<std::string(const std::string&)> fn) {
        substitutor_ = std::move(fn);
        return *this;
    }

    /// Register a hook fired right after the container is created (id assigned),
    /// before any copy-to and before start. A throwing created hook aborts
    /// start() and the partial container is cleaned up. Add several to run them
    /// in registration order. The hook receives the public DockerClient and the
    /// container id, so it can inspect/exec/copy via the existing API.
    GenericImage& with_created_hook(LifecycleHook hook) {
        created_hooks_.push_back(std::move(hook));
        return *this;
    }

    /// Register a hook fired after copy-to and immediately before the container
    /// is started. A throwing starting hook aborts start() and the partial
    /// container is cleaned up. Add several to run them in registration order.
    GenericImage& with_starting_hook(LifecycleHook hook) {
        starting_hooks_.push_back(std::move(hook));
        return *this;
    }

    /// Register a hook fired after the container is started AND has become ready
    /// (wait strategies satisfied), before the handle is returned. A throwing
    /// started hook aborts start() and the container is cleaned up. Add several
    /// to run them in registration order.
    GenericImage& with_started_hook(LifecycleHook hook) {
        started_hooks_.push_back(std::move(hook));
        return *this;
    }

    /// Register a hook fired by the Container when it is being torn down: on an
    /// explicit stop()/remove(), or on destruction of an auto-removing handle,
    /// before the container is removed. Fired exactly once and never on a
    /// persistent (reusable) handle's drop. A throwing stopping hook is swallowed
    /// (teardown is best-effort). Add several to run them in registration order.
    GenericImage& with_stopping_hook(LifecycleHook hook) {
        stopping_hooks_.push_back(std::move(hook));
        return *this;
    }

    /// Retry the whole create→start→wait sequence up to `n` times if an attempt
    /// fails (each retry creates a brand-new container). Values < 1 are treated
    /// as 1 (a single attempt, no retry).
    GenericImage& with_startup_attempts(int n) {
        startup_attempts_ = n < 1 ? 1 : n;
        return *this;
    }

    // --- Getters ---

    const std::string& image() const noexcept { return image_; }
    const std::string& tag() const noexcept { return tag_; }
    const std::vector<ContainerPort>& exposed_ports() const noexcept { return exposed_ports_; }
    const std::vector<std::pair<std::string, std::string>>& env() const noexcept { return env_; }
    const std::vector<std::string>& cmd() const noexcept { return spec_.cmd; }
    const std::vector<std::string>& entrypoint() const noexcept { return spec_.entrypoint; }
    const std::optional<std::string>& working_dir() const noexcept { return spec_.working_dir; }
    const std::optional<std::string>& user() const noexcept { return spec_.user; }
    bool privileged() const noexcept { return spec_.privileged; }
    const std::optional<std::string>& isolation() const noexcept { return spec_.isolation; }
    bool tty() const noexcept { return spec_.tty; }
    const std::vector<Mount>& mounts() const noexcept { return spec_.mounts; }
    const std::vector<CopyToContainer>& copy_to_sources() const noexcept {
        return copy_to_sources_;
    }
    const std::vector<std::pair<std::string, std::string>>& labels() const noexcept {
        return spec_.labels;
    }
    const std::vector<WaitFor>& waits() const noexcept { return waits_; }
    std::chrono::milliseconds startup_timeout() const noexcept { return startup_timeout_; }
    const std::optional<Healthcheck>& healthcheck() const noexcept { return spec_.healthcheck; }
    const std::optional<std::string>& network() const noexcept { return spec_.network; }
    const std::vector<std::string>& network_aliases() const noexcept {
        return spec_.network_aliases;
    }
    const std::optional<std::string>& static_ipv4() const noexcept { return spec_.static_ipv4; }
    const std::optional<std::string>& container_name() const noexcept { return spec_.name; }
    const std::optional<std::string>& platform() const noexcept { return spec_.platform; }
    const std::optional<RegistryAuth>& registry_auth() const noexcept { return registry_auth_; }
    const std::optional<std::int64_t>& memory_limit() const noexcept { return spec_.memory_bytes; }
    const std::optional<std::int64_t>& shm_size() const noexcept { return spec_.shm_size_bytes; }
    const std::vector<Ulimit>& ulimits() const noexcept { return spec_.ulimits; }
    const std::vector<std::string>& cap_add() const noexcept { return spec_.cap_add; }
    const std::vector<std::string>& cap_drop() const noexcept { return spec_.cap_drop; }
    const std::vector<std::string>& extra_hosts() const noexcept { return spec_.extra_hosts; }
    const std::vector<std::uint16_t>& exposed_host_ports() const noexcept {
        return host_access_ports_;
    }
    const std::string& create_body_patch() const noexcept { return spec_.create_body_patch; }
    ImagePullPolicy image_pull_policy() const noexcept { return pull_policy_; }
    bool reuse() const noexcept { return reuse_; }
    const std::function<std::string(const std::string&)>& image_name_substitutor() const noexcept {
        return substitutor_;
    }
    const std::vector<LifecycleHook>& created_hooks() const noexcept { return created_hooks_; }
    const std::vector<LifecycleHook>& starting_hooks() const noexcept { return starting_hooks_; }
    const std::vector<LifecycleHook>& started_hooks() const noexcept { return started_hooks_; }
    const std::vector<LifecycleHook>& stopping_hooks() const noexcept { return stopping_hooks_; }
    int startup_attempts() const noexcept { return startup_attempts_; }

    /// Snapshot this builder into a self-contained `ContainerRequest` — the
    /// fully-translated create spec (resolved/substituted image reference, env
    /// joined to "KEY=VALUE", ports rendered to "6379/tcp" + publish-all) plus
    /// every run-time input (waits, copy-to sources, hooks, pull/reuse/retry
    /// policy). `start()` is exactly `run(to_request())`; call this directly to
    /// tweak the request or to `run()` it on a custom DockerClient.
    ContainerRequest to_request() const;

    /// Create, start, and wait for a container from this image, returning a RAII
    /// handle that removes the container on destruction. Throws on failure
    /// (best-effort removing a container that started but never became ready).
    Container start() const;

private:
    /// Assemble the create spec for this image: a copy of the embedded `spec_`
    /// (which already holds every verbatim create field) with the fields that
    /// need translation filled in — the resolved/substituted image reference,
    /// env joined to "KEY=VALUE", and exposed ports rendered to "6379/tcp" with
    /// publish-all turned on. Session/reuse labels are layered on by `run()`.
    CreateContainerSpec build_spec() const;

    std::string image_;
    std::string tag_;
    /// Domain ports (declared order); rendered at start().
    std::vector<ContainerPort> exposed_ports_;
    /// Domain env; joined to "K=V" at start().
    std::vector<std::pair<std::string, std::string>> env_;

    /// Every verbatim create field (cmd, entrypoint, mounts, labels, host-config
    /// knobs, network, name, platform, …). Setters write here, getters read here,
    /// and start() copies it as the base of the create spec.
    CreateContainerSpec spec_;

    std::vector<CopyToContainer> copy_to_sources_;
    std::vector<std::uint16_t> host_access_ports_; ///< host ports exposed via the sshd sidecar
    std::vector<WaitFor> waits_;
    std::chrono::milliseconds startup_timeout_{std::chrono::seconds(60)};
    std::optional<RegistryAuth> registry_auth_;
    ImagePullPolicy pull_policy_ = ImagePullPolicy::Default;
    std::function<std::string(const std::string&)> substitutor_;
    bool reuse_ = false;
    std::vector<LifecycleHook> created_hooks_;  ///< fired after create, before copy/start
    std::vector<LifecycleHook> starting_hooks_; ///< fired after copy, before start
    std::vector<LifecycleHook> started_hooks_;  ///< fired after wait-until-ready
    std::vector<LifecycleHook> stopping_hooks_; ///< fired by the Container on teardown
    int startup_attempts_ = 1;                  ///< create→start→wait retry count (>=1)
};

} // namespace testcontainers