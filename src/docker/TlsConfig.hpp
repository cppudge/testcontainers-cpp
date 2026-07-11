#pragma once

#include <string>

// Pure TLS-config helpers shared by the TLS transport (Transport.cpp) and the
// unit tests. Kept separate so the resolution logic can be exercised without a
// daemon, mirroring Auth.hpp / HostResolve.hpp. The env/file reads
// (docker_cert_path / docker_tls_verify) are the only impurities;
// resolve_tls_files stays pure.
namespace testcontainers::docker {

/// The three Docker TLS material files inside a cert directory.
struct TlsFiles {
    std::string ca_cert;
    std::string client_cert;
    std::string client_key;
};

/// Resolve the ca.pem / cert.pem / key.pem paths inside `cert_dir` (Docker's
/// fixed names). Empty cert_dir -> all empty.
TlsFiles resolve_tls_files(const std::string& cert_dir);

/// Read DOCKER_CERT_PATH, else the `docker.cert.path` key of
/// ~/.testcontainers.properties (falling back to ~/.docker when TLS verify is
/// on but no path is set). Returns "" when none.
std::string docker_cert_path();

/// DOCKER_TLS_VERIFY truthy (in {1,true,TRUE,True}); when the env var is not
/// set, the `docker.tls.verify` key of ~/.testcontainers.properties decides
/// ("1" or case-insensitive "true", docker-java parity).
bool docker_tls_verify();

} // namespace testcontainers::docker
