#include <iostream>

#include <testcontainers/GenericImage.hpp>
#include <testcontainers/version.hpp>

// Consumer smoke test, run by `conan create` with no Docker daemon around:
// exercises a public header beyond version.hpp (the request builder never
// talks to the daemon) and prints the dependency report, whose libarchive /
// OpenSSL calls prove the transitive link lines of the static library.
int main() {
    using namespace testcontainers;

    GenericImage image("alpine", "3.20");
    image.with_env("TC_SMOKE", "1").with_wait(wait_for::log("ready"));

    std::cout << dependency_report();
    if (version().empty()) {
        std::cerr << "version() is empty\n";
        return 1;
    }
    return 0;
}
