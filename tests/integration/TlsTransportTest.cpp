#include <gtest/gtest.h>

#include <string>

#include "TestEnv.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/docker/DockerHost.hpp"

#include "docker/Transport.hpp"

// Tests in this file (integration; no Docker daemon required):
//   [TC_TLS builds — the default]
//   TlsTransport.HttpsSchemeIsWired - connect() for an https:// host attempts a real TLS connection (no longer "not implemented"); a refused/failed connect throws DockerError.
//   TlsTransport.RealHandshakeRoundTrip - a TlsTransport handshakes with an in-process self-signed TLS echo server and round-trips bytes (DOCKER_TLS_VERIFY unset -> verify_none).
//   TlsTransport.MutualTlsPresentsClientCertificate - a server that REQUIRES a client certificate (what `--tlsverify` daemons do) accepts the cert/key from DOCKER_CERT_PATH; pins the 2026-07-10 regression where the cert was loaded after SSL_new snapshotted the context and the handshake died with "certificate required".
//   TlsTransport.VerifyRejectsUntrustedServer - with DOCKER_TLS_VERIFY=1, a server whose cert is not signed by ca.pem is rejected; pins the verify MODE being inherited by the stream (set after creation it silently stayed fail-open at verify_none).
//   [TC_TLS=OFF builds]
//   TlsTransport.DisabledBuildThrowsClearError - connect() for an https:// host throws a DockerError naming the TC_TLS build option.

using testcontainers::DockerError;
using testcontainers::DockerHost;

#if defined(TC_TLS)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <system_error>
#include <thread>

// Asio first so it pulls <winsock2.h> before any <windows.h> below.
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace asio = boost::asio;

namespace {

// ===== Self-signed cert/key generation via the OpenSSL C API =====
// Writes cert.pem + key.pem into `dir` (Docker's fixed cert/key names) for an
// in-process TLS server. Returns false on any OpenSSL failure (test then skips).
bool write_self_signed_cert(const std::filesystem::path& dir) {
    bool ok = false;
    EVP_PKEY* pkey = nullptr;
    X509* x509 = nullptr;
    FILE* key_fp = nullptr;
    FILE* cert_fp = nullptr;

    // RSA-2048 key.
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) {
        goto done;
    }
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        goto done;
    }

    x509 = X509_new();
    if (!x509) {
        goto done;
    }
    X509_set_version(x509, 2); // X509 v3
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60L * 60L); // valid 1 hour
    X509_set_pubkey(x509, pkey);

    {
        X509_NAME* name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(x509, name); // self-signed
    }
    if (X509_sign(x509, pkey, EVP_sha256()) == 0) {
        goto done;
    }

    key_fp = std::fopen((dir / "key.pem").string().c_str(), "wb");
    if (!key_fp || !PEM_write_PrivateKey(key_fp, pkey, nullptr, nullptr, 0, nullptr, nullptr)) {
        goto done;
    }
    cert_fp = std::fopen((dir / "cert.pem").string().c_str(), "wb");
    if (!cert_fp || !PEM_write_X509(cert_fp, x509)) {
        goto done;
    }
    ok = true;

done:
    if (key_fp) {
        std::fclose(key_fp);
    }
    if (cert_fp) {
        std::fclose(cert_fp);
    }
    if (x509) {
        X509_free(x509);
    }
    if (pkey) {
        EVP_PKEY_free(pkey);
    }
    if (pctx) {
        EVP_PKEY_CTX_free(pctx);
    }
    return ok;
}

using tctest::ScopedEnv;

/// This process's id, for collision-free temp dir names.
unsigned long process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

/// A self-deleting temp directory seeded with a self-signed cert.pem/key.pem
/// pair. `ok` is false (test should skip) when creation or cert generation
/// failed.
struct CertDir {
    explicit CertDir(const std::string& tag) {
        namespace fs = std::filesystem;
        path = fs::temp_directory_path() / (tag + "_" + std::to_string(process_id()));
        std::error_code ec;
        fs::create_directories(path, ec);
        ok = !ec && write_self_signed_cert(path);
    }
    ~CertDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    CertDir(const CertDir&) = delete;
    CertDir& operator=(const CertDir&) = delete;

    std::filesystem::path path;
    bool ok = false;
};

} // namespace

// REQUIRED: proves the Https path is implemented and reachable. No daemon
// needed. The connection should fail (nothing is listening), but with a real
// connect/handshake error — never the old "not implemented". The dead port is
// found by binding an ephemeral listener and closing it — a fixed 2376 would
// collide with a real TLS daemon (e.g. the dind the TLS CI job runs).
TEST(TlsTransport, HttpsSchemeIsWired) {
    asio::io_context probe_ioc;
    asio::ip::tcp::acceptor probe(probe_ioc,
                                  asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t dead_port = probe.local_endpoint().port();
    probe.close();

    try {
        testcontainers::docker::connect(
            DockerHost::parse("https://127.0.0.1:" + std::to_string(dead_port)));
        FAIL() << "expected connect() to throw (nothing is listening on " << dead_port << ")";
    } catch (const DockerError& e) {
        const std::string msg = e.what();
        EXPECT_EQ(msg.find("not implemented"), std::string::npos)
            << "Https transport still reports 'not implemented': " << msg;
    }
}

// BEST-EFFORT: a full TLS handshake + byte round-trip against an in-process
// self-signed echo server. DOCKER_TLS_VERIFY is unset, so the client uses
// verify_none and needs no CA. Skips (never fails flakily) if cert generation or
// the server bring-up does not succeed.
TEST(TlsTransport, RealHandshakeRoundTrip) {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    const fs::path dir = fs::temp_directory_path() / ("tc_tls_" + std::to_string(pid));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        GTEST_SKIP(); // could not create the temp cert dir
    }
    struct DirGuard {
        fs::path p;
        ~DirGuard() {
            std::error_code e;
            fs::remove_all(p, e);
        }
    } guard{dir};

    if (!write_self_signed_cert(dir)) {
        GTEST_SKIP(); // could not generate a self-signed cert via the OpenSSL API
    }

    // The server: bind an ephemeral port, accept one TLS connection, echo bytes.
    asio::io_context server_ioc;
    asio::ip::tcp::acceptor acceptor(
        server_ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::ssl::context server_ctx(asio::ssl::context::tls_server);
    try {
        server_ctx.use_certificate_file((dir / "cert.pem").string(), asio::ssl::context::pem);
        server_ctx.use_private_key_file((dir / "key.pem").string(), asio::ssl::context::pem);
    } catch (const std::exception&) {
        GTEST_SKIP(); // server could not load the generated cert/key
    }

    std::thread server_thread([&] {
        boost::system::error_code sec;
        asio::ssl::stream<asio::ip::tcp::socket> server_stream(server_ioc, server_ctx);
        acceptor.accept(server_stream.lowest_layer(), sec);
        if (sec) {
            return;
        }
        server_stream.handshake(asio::ssl::stream_base::server, sec);
        if (sec) {
            return;
        }
        std::array<char, 64> buf{};
        const std::size_t n = server_stream.read_some(asio::buffer(buf), sec);
        if (sec) {
            return;
        }
        asio::write(server_stream, asio::buffer(buf.data(), n), sec);
        server_stream.shutdown(sec); // best-effort; eof/truncated is normal
    });

    // The client: DOCKER_CERT_PATH points at our dir, but with verify UNSET so
    // the TlsTransport uses verify_none and needs no CA.
    ScopedEnv cert_path("DOCKER_CERT_PATH", dir.string());
    ScopedEnv verify("DOCKER_TLS_VERIFY", std::nullopt);

    std::string error;
    bool round_tripped = false;
    try {
        auto transport = testcontainers::docker::connect(
            DockerHost::parse("https://127.0.0.1:" + std::to_string(port)));

        const std::string payload = "hello-tls";
        boost::system::error_code wec;
        transport->write_some(payload.data(), payload.size(), wec);
        ASSERT_FALSE(wec) << "write failed: " << wec.message();

        std::array<char, 64> rbuf{};
        boost::system::error_code rec;
        const std::size_t n = transport->read_some(rbuf.data(), rbuf.size(), rec);
        ASSERT_FALSE(rec) << "read failed: " << rec.message();

        EXPECT_EQ(std::string(rbuf.data(), n), payload);
        round_tripped = true;
        transport->close();
    } catch (const std::exception& e) {
        error = e.what();
    }

    if (server_thread.joinable()) {
        server_thread.join();
    }

    if (!round_tripped && !error.empty()) {
        FAIL() << "TLS round-trip failed: " << error;
    }
}

// REGRESSION (2026-07-10): a `--tlsverify` daemon REQUIRES the client
// certificate. The in-process server here demands one
// (verify_fail_if_no_peer_cert), trusting the very pair the client presents
// from DOCKER_CERT_PATH. The transport used to load the client cert into the
// SSL context only after the stream was created — SSL_new had already
// snapshotted an empty certificate list, so every handshake against a real
// tlsverify daemon died with "certificate required". DOCKER_TLS_VERIFY stays
// unset: the client side needs no CA/hostname machinery for this pin.
TEST(TlsTransport, MutualTlsPresentsClientCertificate) {
    const CertDir dir("tc_tls_mtls");
    if (!dir.ok) {
        GTEST_SKIP(); // could not create the temp dir / generate the cert
    }

    asio::io_context server_ioc;
    asio::ip::tcp::acceptor acceptor(
        server_ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::ssl::context server_ctx(asio::ssl::context::tls_server);
    try {
        server_ctx.use_certificate_file((dir.path / "cert.pem").string(), asio::ssl::context::pem);
        server_ctx.use_private_key_file((dir.path / "key.pem").string(), asio::ssl::context::pem);
        // Trust exactly the client's (self-signed) cert, and refuse a
        // handshake without one — the dockerd --tlsverify behavior.
        server_ctx.load_verify_file((dir.path / "cert.pem").string());
    } catch (const std::exception&) {
        GTEST_SKIP(); // server could not load the generated cert/key
    }
    server_ctx.set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);

    std::thread server_thread([&] {
        boost::system::error_code sec;
        asio::ssl::stream<asio::ip::tcp::socket> server_stream(server_ioc, server_ctx);
        acceptor.accept(server_stream.lowest_layer(), sec);
        if (sec) {
            return;
        }
        server_stream.handshake(asio::ssl::stream_base::server, sec);
        if (sec) {
            return; // a client presenting no cert aborts here — the test then fails on read
        }
        std::array<char, 64> buf{};
        const std::size_t n = server_stream.read_some(asio::buffer(buf), sec);
        if (sec) {
            return;
        }
        asio::write(server_stream, asio::buffer(buf.data(), n), sec);
        server_stream.shutdown(sec); // best-effort
    });

    ScopedEnv cert_path("DOCKER_CERT_PATH", dir.path.string());
    ScopedEnv verify("DOCKER_TLS_VERIFY", std::nullopt);

    std::string error;
    bool round_tripped = false;
    try {
        auto transport = testcontainers::docker::connect(
            DockerHost::parse("https://127.0.0.1:" + std::to_string(port)));

        const std::string payload = "hello-mtls";
        boost::system::error_code wec;
        transport->write_some(payload.data(), payload.size(), wec);
        ASSERT_FALSE(wec) << "write failed: " << wec.message();

        std::array<char, 64> rbuf{};
        boost::system::error_code rec;
        const std::size_t n = transport->read_some(rbuf.data(), rbuf.size(), rec);
        ASSERT_FALSE(rec) << "read failed: " << rec.message();

        EXPECT_EQ(std::string(rbuf.data(), n), payload);
        round_tripped = true;
        transport->close();
    } catch (const std::exception& e) {
        error = e.what();
    }

    if (server_thread.joinable()) {
        server_thread.join();
    }

    if (!round_tripped) {
        FAIL() << "mutual-TLS round-trip failed: " << error;
    }
}

// With DOCKER_TLS_VERIFY=1 the server certificate must actually be verified:
// a server whose cert is NOT signed by the ca.pem in DOCKER_CERT_PATH has to
// be rejected. Pins the verify MODE being inherited by the stream at creation
// — set on the context after the stream existed, it silently stayed at its
// fail-open verify_none default and this connect() would succeed.
TEST(TlsTransport, VerifyRejectsUntrustedServer) {
    namespace fs = std::filesystem;
    const CertDir server_dir("tc_tls_srv");
    const CertDir client_dir("tc_tls_cli");
    if (!server_dir.ok || !client_dir.ok) {
        GTEST_SKIP(); // could not create the temp dirs / generate the certs
    }
    // The client's ca.pem is its OWN self-signed cert — a CA that did not
    // sign the server's (independently generated) certificate.
    std::error_code copy_ec;
    fs::copy_file(client_dir.path / "cert.pem", client_dir.path / "ca.pem", copy_ec);
    if (copy_ec) {
        GTEST_SKIP();
    }

    asio::io_context server_ioc;
    asio::ip::tcp::acceptor acceptor(
        server_ioc, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
    const std::uint16_t port = acceptor.local_endpoint().port();

    asio::ssl::context server_ctx(asio::ssl::context::tls_server);
    try {
        server_ctx.use_certificate_file((server_dir.path / "cert.pem").string(),
                                        asio::ssl::context::pem);
        server_ctx.use_private_key_file((server_dir.path / "key.pem").string(),
                                        asio::ssl::context::pem);
    } catch (const std::exception&) {
        GTEST_SKIP(); // server could not load the generated cert/key
    }

    std::thread server_thread([&] {
        boost::system::error_code sec;
        asio::ssl::stream<asio::ip::tcp::socket> server_stream(server_ioc, server_ctx);
        acceptor.accept(server_stream.lowest_layer(), sec);
        if (sec) {
            return;
        }
        // The client must abort this handshake (untrusted CA) — any error
        // here is the expected outcome.
        server_stream.handshake(asio::ssl::stream_base::server, sec);
    });

    ScopedEnv cert_path("DOCKER_CERT_PATH", client_dir.path.string());
    ScopedEnv verify("DOCKER_TLS_VERIFY", "1");

    EXPECT_THROW((void)testcontainers::docker::connect(
                     DockerHost::parse("https://127.0.0.1:" + std::to_string(port))),
                 DockerError);

    if (server_thread.joinable()) {
        server_thread.join();
    }
}

#else // TC_TLS

// The Https branch of connect() must fail LOUDLY in a TLS-less build: a clear
// DockerError naming the build option, not an obscure link or protocol error.
TEST(TlsTransport, DisabledBuildThrowsClearError) {
    try {
        testcontainers::docker::connect(DockerHost::parse("https://127.0.0.1:2376"));
        FAIL() << "expected connect() to throw (this build has TC_TLS=OFF)";
    } catch (const DockerError& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("TC_TLS"), std::string::npos) << msg;
    }
}

#endif // TC_TLS
