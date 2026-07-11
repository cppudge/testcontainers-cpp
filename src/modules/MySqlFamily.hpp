#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "testcontainers/GenericImage.hpp"
#include "testcontainers/modules/detail/MySqlFamily.hpp"

// The shared core of the MySQL/MariaDB module pair: everything that must not
// fork between the two flavors (the root-password boot matrix, init-script
// staging, the readiness probes, rendering, the URL) lives here once; the
// public classes are thin flavor-specific shells over it.

namespace testcontainers::modules::detail {

/// The env-contract names and readiness probe that differ between the two
/// images.
struct MySqlFamilyFlavor {
    const char* display_name;         ///< "MySQL" / "MariaDB", for error messages
    const char* env_user;             ///< MYSQL_USER / MARIADB_USER
    const char* env_password;         ///< MYSQL_PASSWORD / MARIADB_PASSWORD
    const char* env_root_password;    ///< MYSQL_ROOT_PASSWORD / MARIADB_ROOT_PASSWORD
    const char* env_allow_empty_root; ///< the two images spell this differently
    const char* env_database;         ///< MYSQL_DATABASE / MARIADB_DATABASE
    std::vector<std::string> (*ready_probe)(const MySqlFamilyOptions& opts);
};

const MySqlFamilyFlavor& mysql_flavor();
const MySqlFamilyFlavor& mariadb_flavor();

/// True for "root" in any letter case (the images refuse MYSQL_USER=root, so
/// a root username switches the boot matrix to root-only provisioning).
bool is_root_username(const std::string& username);

/// Stage a host init script into opts (validated: extension whitelist,
/// NNNN- ordering prefix, 0755 for .sh). Throws Error on rejection.
void add_init_script(MySqlFamilyOptions& opts, std::filesystem::path host_path);

/// Stage in-memory init-script content under `name` (bare file name).
void add_init_script(MySqlFamilyOptions& opts, const std::string& name, std::string content);

/// Stage a host .cnf drop-in for /etc/mysql/conf.d. Throws Error unless the
/// file name ends in ".cnf" (the image's include glob would skip it).
void add_config_file(MySqlFamilyOptions& opts, std::filesystem::path host_cnf);

/// Validation + rendering shared by both public to_generic()s: the managed
/// env matrix appended last, staged copies, command args, the flavor's
/// readiness probe (unless opts.waits overrides), customizers last of all.
/// Throws Error on an invalid configuration, before any daemon contact.
GenericImage render(const MySqlFamilyFlavor& flavor, const GenericImage& base,
                    const MySqlFamilyOptions& opts);

/// `mysql://user[:password]@host:port[/database]` via ConnectionString.
/// Both flavors emit the mysql scheme: MariaDB speaks the MySQL wire
/// protocol, and common URL-parsing clients reject "mariadb://".
std::string family_connection_string(const std::string& username, const std::string& password,
                                     const std::string& host, std::uint16_t port,
                                     const std::string& database);

} // namespace testcontainers::modules::detail
