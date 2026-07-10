#include "Runner.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "HostPortForwarding.hpp"
#include "Reaper.hpp"
#include "Reuse.hpp"
#include "WaitStrategies.hpp"
#include "docker/ApiMapping.hpp"
#include "testcontainers/docker/ContainerSpec.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers {

namespace detail {

namespace {

/// "|<size>|<mtime-ticks>" for one host file, or "|stat-failed" when the
/// metadata cannot be read — the copy itself raises the real error later, so
/// hashing must not fail first.
std::string file_meta(const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (!ec) {
        const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(path, ec);
        if (!ec) {
            // The clock's rep is __int128 on Apple's libc++ (no to_string
            // overload); long long holds ns-since-epoch until year ~2262.
            const auto ticks = static_cast<long long>(mtime.time_since_epoch().count());
            return "|" + std::to_string(size) + "|" + std::to_string(ticks);
        }
    }
    return "|stat-failed";
}

/// Append one copy-to source to the reuse canonical: target + mode, then the
/// content identity. Byte sources contribute their bytes. Host sources
/// contribute the path PLUS per-file size+mtime, so editing a fixture in
/// place changes the hash without re-reading content (full content hashing
/// stays rejected — it would re-read the whole tree on every start). The
/// directory walk mirrors the tar walk (docker/Tar.cpp): file symlinks are
/// followed, directory symlinks are recorded but not descended, other entry
/// kinds are skipped, and entries are sorted (iteration order is unspecified).
void append_source_canonical(std::string& canonical, const CopyToContainer& s) {
    canonical += "\n" + s.target() + "\n" + std::to_string(s.mode()) + "\n";
    if (!s.is_file() && !s.is_dir()) {
        canonical += s.bytes();
        return;
    }

    canonical += s.host_path().string();
    std::error_code ec;
    if (!std::filesystem::is_directory(s.host_path(), ec)) {
        canonical += file_meta(s.host_path());
        return;
    }

    std::vector<std::string> entries;
    std::filesystem::recursive_directory_iterator it(s.host_path(), ec);
    bool walk_failed = static_cast<bool>(ec);
    while (!walk_failed && it != std::filesystem::recursive_directory_iterator{}) {
        // u8string keeps names byte-stable regardless of the host code page
        // (same reasoning as the tar walk).
        const std::u8string rel8 = it->path().lexically_relative(s.host_path()).generic_u8string();
        const std::string rel(rel8.begin(), rel8.end());
        std::error_code type_ec;
        if (it->is_directory(type_ec)) {
            entries.push_back(rel + "/"); // presence matters: empty dirs are copied
        } else if (it->is_regular_file(type_ec)) {
            entries.push_back(rel + file_meta(it->path()));
        }
        // Checked HERE (not at the loop top): a failed increment may also land
        // the iterator on end, which would skip a top-of-loop check and lose
        // the marker.
        it.increment(ec);
        if (ec) {
            walk_failed = true;
        }
    }
    if (walk_failed) {
        canonical += "|walk-failed"; // the copy raises the real error later
    }
    std::sort(entries.begin(), entries.end());
    for (const std::string& entry : entries) {
        canonical += "\n" + entry;
    }
}

} // namespace

Container Runner::run(DockerClient& client, const ContainerRequest& request) {
    // Local copy of the create spec: the session/reuse labels layered on below
    // depend on this run, not on the request.
    CreateContainerSpec spec = request.spec;

    // ImagePullPolicy::Always pulls before create even when the image is present
    // locally; Default relies on create's lazy pull-on-404 path.
    if (request.pull_policy == ImagePullPolicy::Always) {
        client.pull_image(spec.image, request.registry_auth);
    }

    // Host-port exposure: make sure the sshd sidecar + SSH forwards are up and
    // point host.testcontainers.internal at the sidecar (an ExtraHosts entry
    // appended to this run's spec). Must happen before the reuse hash below —
    // reachability is part of the container's effective config. The sidecar's
    // own start() recurses through here with no host_access_ports, so this
    // cannot loop.
    if (!request.host_access_ports.empty()) {
        detail::HostPortForwarder::instance().wire(client, spec, request.host_access_ports);
    }

    // The shared create→copy→start→wait tail, returning a handle. `remove_on_drop`
    // is false for reusable containers (they must persist across runs). A throwing
    // created/starting/started hook is treated like any other failure here: it is
    // inside the try so the partial container is best-effort removed before the
    // exception propagates (no leak at any of the three points).
    const auto create_start_wait = [&](const CreateContainerSpec& s,
                                       bool remove_on_drop) -> Container {
        const std::string id = client.create_container(s, request.registry_auth);

        // Everything from here until the container is constructed must, on any
        // throw, best-effort remove the partially-created container so it never
        // leaks — including a throwing created/starting/started hook.
        try {
            // created hooks: container exists (id assigned), before copy-to/start.
            for (const LifecycleHook& hook : request.created_hooks) {
                if (hook) {
                    hook(client, id);
                }
            }

            // Copy files/data in after create, before start (the create→copy→start
            // order). One batched PUT carries the whole copy set (an empty set
            // skips the round-trip).
            client.copy_to_container(id, request.copy_to_sources);

            // starting hooks: after copy-to, immediately before start.
            for (const LifecycleHook& hook : request.starting_hooks) {
                if (hook) {
                    hook(client, id);
                }
            }

            client.start_container(id);

            detail::wait_until_ready(client, id, request.waits, request.startup_timeout, spec.tty);

            // started hooks: after wait-until-ready, before constructing the handle.
            for (const LifecycleHook& hook : request.started_hooks) {
                if (hook) {
                    hook(client, id);
                }
            }
        } catch (...) {
            // A container that was created but failed to come up (copy, start,
            // readiness, or a throwing created/starting/started hook) must not leak.
            try {
                client.remove_container(id, /*force*/ true, /*remove_volumes*/ true);
            } catch (...) {
                ;
            }
            throw;
        }

        // The handle gets its own client copy so the caller's `client` stays
        // usable across startup-attempt retries (DockerClient is a stateless host
        // config — opening a fresh connection per call).
        return Container(client, id, remove_on_drop, spec.tty, request.stopping_hooks,
                         request.exposed_ports);
    };

    // Run an attempt-producing factory up to startup_attempts times: on a thrown
    // failure, if attempts remain, swallow and retry (each retry builds a brand-new
    // container via the factory); after the final attempt, rethrow the last error.
    const auto with_retry = [&](const auto& attempt) -> Container {
        const int attempts = std::max(1, request.startup_attempts);
        for (int i = 0;; ++i) {
            try {
                return attempt();
            } catch (...) {
                if (i + 1 >= attempts) {
                    throw; // final attempt failed: propagate the last error
                }
                // attempts remain: create_start_wait already removed the partial
                // container before throwing, so just retry with a fresh one.
            }
        }
    };

    // Reuse is a safety-gated opt-in: it only activates when enabled globally
    // (~/.testcontainers.properties or TESTCONTAINERS_REUSE_ENABLE); otherwise
    // request.reuse degrades to a normal (reaped, auto-removed) container.
    const bool use_reuse = request.reuse && detail::reuse_enabled();
    if (use_reuse) {
        // Canonical config for the hash: the create body WITHOUT any reuse/session
        // labels, plus the copy-to descriptors (target, mode, and the content
        // identity — bytes verbatim; host paths with per-file size+mtime for
        // freshness, see append_source_canonical).
        std::string canonical = docker::build_create_body(spec).dump();
        for (const CopyToContainer& s : request.copy_to_sources) {
            append_source_canonical(canonical, s);
        }
        const std::string hash = detail::reuse_hash(canonical);

        // Reuse containers are NOT session-reaped (they must survive); tag
        // managed-by + the reuse hash only (no session-id label, or Ryuk would
        // reap them).
        spec.labels.emplace_back("org.testcontainers.managed-by", "testcontainers");
        spec.labels.emplace_back(detail::reuse_hash_label(), hash);

        // Look for a running container already matching this hash.
        const auto matches = client.list_containers(
            {{"label", std::string(detail::reuse_hash_label()) + "=" + hash}}, /*all*/ false);
        for (const ContainerSummary& m : matches) {
            if (m.state == "running") {
                // Adopt it: wait for readiness, return a NON-removing handle. This
                // path is NOT retried — it adopts an already-running match rather
                // than creating anything.
                detail::wait_until_ready(client, m.id, request.waits, request.startup_timeout,
                                         spec.tty);
                return Container(client, m.id, /*remove_on_drop*/ false, spec.tty,
                                 request.stopping_hooks, request.exposed_ports);
            }
        }
        // No match: create a NEW reuse container (persistent, not reaped). This
        // creates, so it IS subject to startup-attempt retry.
        return with_retry([&] { return create_start_wait(spec, /*remove_on_drop*/ false); });
    }

    // Normal path: tag the container so Ryuk (and tooling) can find it
    // (managed-by + session), and auto-remove the handle on drop. Subject to
    // startup-attempt retry (each retry creates a brand-new container).
    for (const auto& label : detail::testcontainers_labels()) {
        spec.labels.push_back(label);
    }
    return with_retry([&] { return create_start_wait(spec, /*remove_on_drop*/ true); });
}

} // namespace detail

Container run(const ContainerRequest& request) {
    // Make sure the crash-safety reaper is up before we create anything it should
    // reap (no-op if Ryuk is disabled). The run's own client doubles as the
    // reaper's daemon reference — one environment resolve for both.
    DockerClient client = DockerClient::from_environment();
    detail::Reaper::instance().ensure_started(client);
    return detail::Runner::run(client, request);
}

Container run(DockerClient client, const ContainerRequest& request) {
    // Boot the reaper on the daemon the CALLER chose, not the environment one —
    // otherwise a remote-endpoint run would start Ryuk locally (or fail with no
    // local daemon) and the remote containers would never be watched. Each
    // daemon gets its own reaper (keyed by endpoint), so mixing this with
    // environment-daemon runs in one process watches both.
    detail::Reaper::instance().ensure_started(client);
    return detail::Runner::run(client, request);
}

} // namespace testcontainers
