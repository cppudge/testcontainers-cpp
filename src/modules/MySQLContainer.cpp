#include "testcontainers/modules/MySQLContainer.hpp"

#include <string>
#include <utility>

#include "MySqlFamily.hpp"

namespace testcontainers::modules {

MySQLContainer::MySQLContainer()
    : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    // Pin, port, and the doubled startup budget are baked once; the
    // credential env matrix and the readiness probe depend on the
    // credentials, so they render at to_generic() time (in the shared
    // detail::render — see MySqlFamily.cpp for the boot-matrix and
    // temporary-bootstrap-server rationale).
    image_.with_exposed_port(tcp(kPort)).with_startup_timeout(std::chrono::seconds(120));
}

MySQLContainer& MySQLContainer::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

MySQLContainer& MySQLContainer::with_username(std::string username) {
    // Normalized: the real account is 'root' (account names are
    // case-sensitive), so a "Root" spelling must not leak into the probe,
    // the getters, or the DSN.
    opts_.username = detail::is_root_username(username) ? "root" : std::move(username);
    return *this;
}

MySQLContainer& MySQLContainer::with_init_script(std::filesystem::path host_path) {
    detail::add_init_script(opts_, std::move(host_path));
    return *this;
}

MySQLContainer& MySQLContainer::with_init_script(const std::string& name, std::string content) {
    detail::add_init_script(opts_, name, std::move(content));
    return *this;
}

MySQLContainer& MySQLContainer::with_config_file(std::filesystem::path host_cnf) {
    detail::add_config_file(opts_, std::move(host_cnf));
    return *this;
}

// Out of line so the header needs no Network definition.
MySQLContainer& MySQLContainer::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

GenericImage MySQLContainer::to_generic() const {
    return detail::render(detail::mysql_flavor(), image_, opts_);
}

StartedMySQL MySQLContainer::start() const {
    return StartedMySQL(to_generic().start(), opts_.username, opts_.password, opts_.database);
}

std::string StartedMySQL::connection_string() const {
    return detail::family_connection_string(username_, password_, host_, port_, database_);
}

} // namespace testcontainers::modules
