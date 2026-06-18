#include <iostream>

#include "testcontainers/testcontainers.hpp"

// Minimal end-to-end smoke check: if this builds, links, and runs, then the
// Conan deps (Boost/Beast, nlohmann_json, OpenSSL, libarchive) and the
// cmake-conan toolchain are all correctly wired into the library.
int main() {
    std::cout << testcontainers::dependency_report();
    return 0;
}
