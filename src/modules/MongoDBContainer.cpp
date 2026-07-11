#include "testcontainers/modules/MongoDBContainer.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "testcontainers/ConnectionString.hpp"
#include "testcontainers/Error.hpp"
#include "testcontainers/WaitFor.hpp"
#include "testcontainers/docker/DockerClient.hpp"

namespace testcontainers::modules {

namespace {

/// [A-Za-z0-9_-] only: the name is single-quoted into the initiate JS, and
/// this alphabet is what keeps that sound (it is also what mongod accepts).
bool valid_replica_set_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// Post-start choreography: initiate the set, then wait for the election.
/// This cannot be a wait strategy — the Runner runs all waits BEFORE the
/// started hooks, so a PRIMARY probe would poll before rs.initiate existed.
/// Until this hook returns, the node rejects writes (NotWritablePrimary).
void run_mongo_rs_init(DockerClient& client, const std::string& id, const std::string& rs_name) {
    // The member host is 127.0.0.1 on purpose: it satisfies mongod's
    // initiate-time self-check deterministically (no dependency on the
    // container hostname or an inspect round-trip), and no client ever
    // routes by it — the DSN pins directConnection=true, so drivers never
    // chase the advertised member list. AlreadyInitialized is tolerated so
    // a pre-initiated data directory (a mounted volume) is not a failure.
    const std::string initiate_js =
        "try { rs.initiate({_id: '" + rs_name +
        "', members: [{_id: 0, host: '127.0.0.1:27017'}]}); } "
        "catch (e) { if (e.codeName !== 'AlreadyInitialized') throw e; }";
    const ExecResult initiate = client.exec(id, {"mongosh", "--quiet", "--eval", initiate_js});
    if (initiate.exit_code != 0) {
        throw DockerError(
            "rs.initiate failed (exit " + std::to_string(initiate.exit_code) + "): " +
                (initiate.stderr_data.empty() ? initiate.stdout_data : initiate.stderr_data),
            std::nullopt, id);
    }

    // The single-node election normally completes well under 2s; the
    // in-shell cap (200 x 100ms ~ 20s) is headroom for cold CI. hello() is
    // wrapped so a transient error during the step-up keeps the poll going
    // instead of failing start() spuriously. One exec, no stdin, no TTY —
    // works on every transport.
    const ExecResult primary =
        client.exec(id, {"mongosh", "--quiet", "--eval",
                         "let n = 0; while (true) { let ok = false; "
                         "try { ok = db.hello().isWritablePrimary; } catch (e) {} "
                         "if (ok) break; if (n++ >= 200) quit(1); sleep(100); }"});
    if (primary.exit_code != 0) {
        throw StartupTimeoutError(
            "mongodb node did not become the writable PRIMARY within ~20s of rs.initiate", id);
    }
}

} // namespace

MongoDBContainer::MongoDBContainer()
    : image_(GenericImage::from_reference(std::string(kDefaultImage))) {
    image_.with_exposed_port(tcp(kPort));
}

MongoDBContainer& MongoDBContainer::with_image(const std::string& reference) {
    image_.with_image(reference);
    return *this;
}

// Out of line so the header needs no Network definition.
MongoDBContainer& MongoDBContainer::with_network(const Network& network) {
    image_.with_network(network);
    return *this;
}

GenericImage MongoDBContainer::to_generic() const {
    if (!valid_replica_set_name(replica_set_name_)) {
        // Fail fast: the name lands on the mongod command line and inside
        // the single-quoted initiate JS.
        throw Error("replica set name \"" + replica_set_name_ +
                    "\" must be non-empty and use only letters, digits, '-' and '_'");
    }

    // Render into a COPY: repeated to_generic()/start() calls must never
    // accumulate state in the config.
    GenericImage generic = image_;

    // The entrypoint prepends "mongod" when the first arg starts with '-'.
    // --bind_ip_all is stated explicitly (mongod defaults to localhost-only;
    // the entrypoint would add it, but stating it removes the dependency on
    // entrypoint behavior). Deliberately NO MONGO_INITDB_* env: it would
    // make the entrypoint boot a temporary localhost-only server first (the
    // log line then appears twice and the wait releases on the throwaway
    // instance), and auth without a cluster keyfile refuses to start under
    // --replSet at all.
    generic.with_cmd({"--replSet", replica_set_name_, "--bind_ip_all"});

    // Exact casing of the 4.4+ structured-log message; appears exactly once
    // because nothing triggers the entrypoint's initdb phase. The port wait
    // then proves the HOST side of the mapping before the DSN is handed out.
    generic.with_wait(wait_for::log("Waiting for connections"));
    generic.with_wait(wait_for::listening_port(tcp(kPort)));

    const std::string rs_name = replica_set_name_;
    generic.with_started_hook([rs_name](DockerClient& client, const std::string& id) {
        run_mongo_rs_init(client, id, rs_name);
    });

    // Customizers last: what the escape hatch sets wins over the module's
    // rendering (the header documents which parts must not be replaced).
    for (const auto& customize : customizers_) {
        customize(generic);
    }
    return generic;
}

StartedMongoDB MongoDBContainer::start() const {
    return StartedMongoDB(to_generic().start(), replica_set_name_, database_);
}

std::string StartedMongoDB::connection_string(const std::string& database) const {
    ConnectionString url("mongodb");
    url.with_host(host_).with_port(port_);
    // The database segment is ALWAYS set, even when empty: a MongoDB URI is
    // invalid without the '/' between host and options, and strict parsers
    // (libmongoc, PyMongo) reject mongodb://h:p?directConnection=true.
    url.with_database(database);
    url.with_param("directConnection", "true");
    return url.to_string();
}

ExecResult StartedMongoDB::mongosh(const std::string& js) const {
    // The positional argument selects the snippet's default database; an
    // empty configured name falls back to the server default ("test").
    if (database_.empty()) {
        return container_.exec({"mongosh", "--quiet", "--eval", js});
    }
    return container_.exec({"mongosh", "--quiet", database_, "--eval", js});
}

} // namespace testcontainers::modules
