#pragma once

#include <functional>
#include <string>
#include <vector>

#include "testcontainers/CopyToContainer.hpp"
#include "testcontainers/WaitFor.hpp"

namespace testcontainers {
class GenericImage;
}

namespace testcontainers::modules::detail {

/// Implementation detail shared by MySQLContainer and MariaDBContainer: the
/// bag of domain knobs both builders collect. Not part of the public API —
/// the field set may change without notice; configure through the containers'
/// with_* setters.
struct MySqlFamilyOptions {
    std::string username{"test"};
    std::string password{"test"};
    std::string database{"test"};
    /// Init scripts already staged as copies targeting
    /// /docker-entrypoint-initdb.d/NNNN-<name> (NNNN = registration index).
    std::vector<CopyToContainer> init_scripts;
    /// Server options ("--key=value") forwarded to the server binary.
    std::vector<std::string> command_args;
    /// Config drop-ins already staged as copies targeting /etc/mysql/conf.d.
    std::vector<CopyToContainer> config_files;
    /// Custom readiness strategies; empty = the flavor's default probe.
    std::vector<WaitFor> waits;
    /// Escape-hatch callbacks, run last over the rendered GenericImage.
    std::vector<std::function<void(GenericImage&)>> customizers;
};

} // namespace testcontainers::modules::detail
