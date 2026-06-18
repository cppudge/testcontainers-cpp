#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/Container.hpp"
#include "testcontainers/ContainerPort.hpp"
#include "testcontainers/Healthcheck.hpp"
#include "testcontainers/Mount.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers {

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
    const std::vector<std::pair<std::string, std::string>>& labels() const noexcept {
        return labels_;
    }
    const std::vector<WaitFor>& waits() const noexcept { return waits_; }
    std::chrono::milliseconds startup_timeout() const noexcept { return startup_timeout_; }
    const std::optional<Healthcheck>& healthcheck() const noexcept { return healthcheck_; }

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
    std::vector<std::pair<std::string, std::string>> labels_;
    std::vector<WaitFor> waits_;
    std::chrono::milliseconds startup_timeout_{std::chrono::seconds(60)};
    std::optional<Healthcheck> healthcheck_;
};

} // namespace testcontainers
