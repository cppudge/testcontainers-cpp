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

} // namespace

// REQUIRED: proves the Https path is implemented and reachable. No daemon
// needed. The connection to 127.0.0.1:2376 should fail (nothing is listening),
// but with a real connect/handshake error — never the old "not implemented".
TEST(TlsTransport, HttpsSchemeIsWired) {
    try {
        testcontainers::docker::connect(DockerHost::parse("https://127.0.0.1:2376"));
        FAIL() << "expected connect() to throw (nothing is listening on 2376)";
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
