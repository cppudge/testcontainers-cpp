# ===== Optional features =====
# Declared before the Conan provider block below because they shape the
# DEPENDENCY GRAPH, not just the build: disabling TC_HOST_PORT_FORWARDING drops
# libssh2, and disabling BOTH drops OpenSSL entirely (libssh2's crypto backend
# is OpenSSL, so TC_TLS=OFF alone still leaves libcrypto in). The library
# builds and runs without them; the disabled paths throw a DockerError naming
# the option instead (https:// hosts / with_exposed_host_port).
option(TC_TLS "TLS transport for https:// Docker hosts (needs OpenSSL)" ON)
option(TC_HOST_PORT_FORWARDING
    "Host-port exposure via the sshd sidecar tunnel (needs libssh2 + OpenSSL)" ON)

# ===== Conan integration (cmake-conan provider) =====
# When configured directly (not through `conan build`), the vendored cmake-conan
# provider runs `conan install` automatically. conanfile.py sets
# SKIP_CONAN_PROVIDER_CMAKE=ON so this block is skipped during a conan-driven build.
if(DEFINED SKIP_CONAN_PROVIDER_CMAKE)
    message(STATUS "Skipping cmake-conan provider (SKIP_CONAN_PROVIDER_CMAKE is set)")
else()
    message(STATUS "Using cmake-conan provider for automatic conan install")
    # Use a project-local Conan home at the PROJECT ROOT (./.conan2), isolating
    # this project's remotes + package cache from the global Conan home. On by
    # default; set OFF to use the global home. A fresh local home auto-initializes
    # with only the public `conancenter` remote. (An explicit CONAN_HOME env var,
    # if set, always wins.)
    option(TC_LOCAL_CONAN_HOME "Use a project-local .conan2 home at the project root" ON)
    if(TC_LOCAL_CONAN_HOME AND NOT DEFINED ENV{CONAN_HOME})
        file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/.conan2")
        set(ENV{CONAN_HOME} "${CMAKE_SOURCE_DIR}/.conan2")
    endif()
    set(CONAN_HOST_PROFILE "auto-cmake" CACHE STRING "Conan host profile")
    set(CONAN_BUILD_PROFILE "auto-cmake" CACHE STRING "Conan build profile")
    # Mirror the feature options into conanfile.py options so the provider's
    # `conan install` trims the graph to match (otherwise it would still fetch
    # openssl/libssh2 the build never links). Claiming the cache variable
    # before the provider does keeps its "--build=missing" default; the
    # list(APPEND) shadows it with a directory-scope value the provider reads
    # at project() time. `&:` scopes an option to the root conanfile.
    set(CONAN_INSTALL_ARGS "--build=missing" CACHE STRING
        "Command line arguments for conan install")
    if(NOT TC_TLS)
        list(APPEND CONAN_INSTALL_ARGS -o "&:tls=False")
    endif()
    if(NOT TC_HOST_PORT_FORWARDING)
        list(APPEND CONAN_INSTALL_ARGS -o "&:host_port_forwarding=False")
    endif()
    set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES
        "${CMAKE_SOURCE_DIR}/cmake/cmake-conan/conan_provider.cmake")
endif()

# ===== Project =====
cmake_minimum_required(VERSION 3.20)
# The ONE place the version lives. version.cpp compiles it in (see the
# set_source_files_properties below), conanfile.py's set_version() parses it
# out, and project() takes the numeric prefix (CMake versions cannot carry a
# "-alpha.N" prerelease suffix).
set(TC_VERSION_FULL "0.1.0")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" TC_VERSION_NUMERIC "${TC_VERSION_FULL}")
project(testcontainers VERSION ${TC_VERSION_NUMERIC} LANGUAGES CXX)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(TC_BUILD_EXAMPLES "Build examples" ON)
# A package build runs only the unit suite (integration wants a Docker daemon
# the build machine cannot assume), so the Conan recipes pin this OFF and skip
# compiling the ~20 integration TUs they would never run. Repo builds keep both.
option(TC_BUILD_INTEGRATION_TESTS "Build the Docker-daemon integration test suite" ON)

if(BUILD_SHARED_LIBS)
    if(WIN32)
        # The sources carry no dllexport macros, so a Windows DLL would export
        # no symbols and consumers could not link against it. Fail loudly
        # instead of producing an unusable binary.
        message(FATAL_ERROR "BUILD_SHARED_LIBS is not supported on Windows "
                            "(the library has no symbol export macros yet)")
    endif()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    # Default to the dynamic release CRT for a standalone build, but do NOT override
    # a value a consumer already set (e.g. Conan's CMakeToolchain, which selects
    # MultiThreadedDebugDLL for a Debug consumer) — otherwise this static lib is MD
    # while a Debug consumer is MDd and the link fails with LNK2038.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()
if(WIN32)
    # Boost.Asio wants the target Windows version; without it every Asio TU
    # warns "Please define _WIN32_WINNT..." and falls back to a Windows 7
    # target. 0x0A00 = Windows 10, the oldest OS a supported Docker engine
    # runs on.
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# ===== Warnings =====
# High warning level on OUR targets only (dependency headers already arrive as
# system includes — /external:I under MSVC, -isystem under gcc/clang — so they
# stay silent). TC_WERROR hardens warnings into errors; CI turns it on, the
# default stays OFF so a future compiler's new warnings never break a user's
# build of a vendored copy.
option(TC_WERROR "Treat compiler warnings as errors" OFF)
function(tc_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(TC_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
        if(TC_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ===== Dependencies (provided by Conan / CMakeDeps) =====
find_package(Boost REQUIRED)            # header-only Beast + Asio
find_package(nlohmann_json REQUIRED)
find_package(LibArchive REQUIRED)
if(TC_TLS OR TC_HOST_PORT_FORWARDING)
    # TLS transport and/or libssh2's crypto backend + the direct
    # OPENSSL_init_crypto/OPENSSL_thread_stop calls in HostPortForwarding.cpp.
    find_package(OpenSSL REQUIRED)
endif()
if(TC_HOST_PORT_FORWARDING)
    # Case matters: Conan generates Libssh2Config.cmake (capital L) — the
    # lowercase name only resolves on case-insensitive filesystems (Windows).
    find_package(Libssh2 REQUIRED)      # host-port-exposure sidecar tunnel
endif()

# The enabled features, as compile definitions for OUR translation units (the
# macros never appear in public headers — the optional deps stay private
# implementation details, and consumers need no defines at all).
set(TC_FEATURE_DEFINES "")
if(TC_TLS)
    list(APPEND TC_FEATURE_DEFINES TC_TLS)
endif()
if(TC_HOST_PORT_FORWARDING)
    list(APPEND TC_FEATURE_DEFINES TC_HOST_PORT_FORWARDING)
endif()

# ===== Library =====
add_library(testcontainers
    src/version.cpp
    src/docker/DockerHost.cpp
    src/docker/Transport.cpp
    src/docker/ApiMapping.cpp
    src/docker/Auth.cpp
    src/docker/LogDemux.cpp
    src/docker/Tar.cpp
    src/docker/DockerIgnore.cpp
    src/docker/DockerClient.cpp
    src/WaitStrategies.cpp
    src/GenericImage.cpp
    src/GenericBuildableImage.cpp
    src/Runner.cpp
    src/HostPortForwarding.cpp
    src/Container.cpp
    src/Network.cpp
    src/Volume.cpp
    src/Reaper.cpp
    src/Reuse.cpp
    src/DockerComposeContainer.cpp
    src/Process.cpp
    src/compose/ComposeCommand.cpp
    src/compose/ComposeClients.cpp
)
add_library(testcontainers::testcontainers ALIAS testcontainers)
# Only version.cpp sees the version macro, so a version bump relinks one
# object instead of rebuilding the world.
set_source_files_properties(src/version.cpp PROPERTIES
    COMPILE_DEFINITIONS TC_VERSION_STRING="${TC_VERSION_FULL}")
tc_apply_warnings(testcontainers)
if(MSVC)
    # The library reads env vars via portable std::getenv (the _s variant is
    # Windows-only); silence the CRT's C4996 deprecation. PRIVATE: our TUs
    # only. (Defining the reserved identifier in source files instead would
    # trip clang-tidy's bugprone-reserved-identifier.)
    target_compile_definitions(testcontainers PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_include_directories(testcontainers
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src   # internal headers, e.g. "docker/Transport.hpp"
)
target_compile_features(testcontainers PUBLIC cxx_std_20)

# Third-party deps are implementation details: keep them PRIVATE so the public
# headers stay free of Boost/OpenSSL/json/libarchive types. Boost::headers,
# not the umbrella boost::boost: Beast/Asio/System are header-only, and against
# a COMPILED Boost (dependency options are the consumer's, e.g. on ConanCenter)
# the umbrella would drag every built libboost_* into consumers' link lines.
target_link_libraries(testcontainers PRIVATE
    Boost::headers
    nlohmann_json::nlohmann_json
    LibArchive::LibArchive
)
target_compile_definitions(testcontainers PRIVATE ${TC_FEATURE_DEFINES})
if(TC_TLS)
    target_link_libraries(testcontainers PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(TC_HOST_PORT_FORWARDING)
    # OpenSSL::Crypto explicitly, not just via libssh2: HostPortForwarding.cpp
    # itself calls OPENSSL_init_crypto/OPENSSL_thread_stop.
    target_link_libraries(testcontainers PRIVATE Libssh2::libssh2 OpenSSL::Crypto)
endif()

if(WIN32)
    # Asio named-pipe / TCP transport.
    target_link_libraries(testcontainers PRIVATE ws2_32 mswsock)
    if(TC_TLS OR TC_HOST_PORT_FORWARDING)
        # OpenSSL's Windows certificate store.
        target_link_libraries(testcontainers PRIVATE crypt32)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(testcontainers PRIVATE Threads::Threads)
endif()

# ===== Examples =====
if(TC_BUILD_EXAMPLES)
    add_executable(tc_smoke examples/smoke.cpp)
    target_link_libraries(tc_smoke PRIVATE testcontainers::testcontainers)
    tc_apply_warnings(tc_smoke)

    add_executable(tc_ping examples/ping.cpp)
    target_link_libraries(tc_ping PRIVATE testcontainers::testcontainers)
    tc_apply_warnings(tc_ping)
endif()

# ===== Tests =====
include(CTest)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    # Unit tests — no Docker required.
    add_executable(tc_unit_tests
        tests/unit/version_test.cpp
        tests/unit/DockerHostTest.cpp
        tests/unit/HostResolveTest.cpp
        tests/unit/ResponseTest.cpp
        tests/unit/CannedHttpServerTest.cpp
        tests/unit/CopyWireTest.cpp
        tests/unit/BodyLimitTest.cpp
        tests/unit/BuildWireTest.cpp
        tests/unit/DockerIgnoreTest.cpp
        tests/unit/ApiMappingTest.cpp
        tests/unit/ApiVersionTest.cpp
        tests/unit/AuthTest.cpp
        tests/unit/LogDemuxTest.cpp
        tests/unit/StreamReadTest.cpp
        tests/unit/TransportTimeoutTest.cpp
        tests/unit/TransportHalfCloseTest.cpp
        tests/unit/TransportExchangeTest.cpp
        tests/unit/ExecWireTest.cpp
        tests/unit/ErrorModelTest.cpp
        tests/unit/PullRetryTest.cpp
        tests/unit/RunnerTest.cpp
        tests/unit/DockerClientSessionTest.cpp
        tests/unit/ContainerPortTest.cpp
        tests/unit/PortsTest.cpp
        tests/unit/WaitForTest.cpp
        tests/unit/HealthcheckTest.cpp
        tests/unit/MountTest.cpp
        tests/unit/GenericImageTest.cpp
        tests/unit/ProcessTest.cpp
        tests/unit/ReaperTest.cpp
        tests/unit/ReuseTest.cpp
        tests/unit/TarTest.cpp
        tests/unit/DockerComposeContainerTest.cpp
        tests/unit/ComposeCommandTest.cpp
        tests/unit/TlsConfigTest.cpp
    )
    target_link_libraries(tc_unit_tests PRIVATE
        testcontainers::testcontainers
        nlohmann_json::nlohmann_json
        # TarTest builds tars via the internal builder and reads them back with libarchive.
        LibArchive::LibArchive
        # StreamReadTest drives the internal Beast streaming loop over a fake
        # ITransport; TransportTimeoutTest runs in-process Asio loopback servers
        # (allowed in a test translation unit — not a public header).
        Boost::headers
        GTest::gtest
        GTest::gtest_main
    )
    # ApiMappingTest / AuthTest / TarTest include internal headers under src/;
    # tests/support holds helpers shared with the integration tree (TestEnv.hpp).
    target_include_directories(tc_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
    )
    # version_test's dependency-report expectations follow the feature set.
    target_compile_definitions(tc_unit_tests PRIVATE ${TC_FEATURE_DEFINES})
    tc_apply_warnings(tc_unit_tests)
    add_test(NAME unit COMMAND tc_unit_tests)
    set_tests_properties(unit PROPERTIES LABELS "unit")

    if(MSVC)
        # Tests call portable getenv/fopen on purpose (the _s variants are
        # Windows-only); silence the CRT's C4996 deprecation for them.
        target_compile_definitions(tc_unit_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    # Integration tests — require a reachable Docker daemon (skipped otherwise).
    if(TC_BUILD_INTEGRATION_TESTS)
        add_executable(tc_integration_tests
            tests/integration/ContainerLifecycleTest.cpp
            tests/integration/LifecycleTest.cpp
            tests/integration/ContainerLogsTest.cpp
            tests/integration/ContainerConfigTest.cpp
            tests/integration/PortGettersTest.cpp
            tests/integration/RedisMvpTest.cpp
            tests/integration/WaitStrategiesTest.cpp
            tests/integration/ExecTest.cpp
            tests/integration/TtyTest.cpp
            tests/integration/CopyTest.cpp
            tests/integration/BuildImageTest.cpp
            tests/integration/NetworkTest.cpp
            tests/integration/VolumeTest.cpp
            tests/integration/ReaperTest.cpp
            tests/integration/ReuseTest.cpp
            tests/integration/AuthTest.cpp
            tests/integration/HostAccessTest.cpp
            tests/integration/WindowsContainerTest.cpp
            tests/integration/ComposeTest.cpp
            tests/integration/TlsTransportTest.cpp
        )
        target_link_libraries(tc_integration_tests PRIVATE
            testcontainers::testcontainers
            # RedisMvpTest / ReaperTest open raw TCP sockets with Boost.Asio (allowed
            # in a test translation unit — it is not a public header).
            Boost::headers
            # ReaperTest parses raw inspect JSON to read back a container's labels.
            nlohmann_json::nlohmann_json
            GTest::gtest
            GTest::gtest_main
        )
        # TlsTransportTest / HostAccessTest swap their suites for the
        # disabled-path checks when a feature is off (see the #if in each file).
        target_compile_definitions(tc_integration_tests PRIVATE ${TC_FEATURE_DEFINES})
        if(TC_TLS)
            # TlsTransportTest runs an in-process Boost.Asio TLS echo server and
            # generates a self-signed cert/key via the OpenSSL C API.
            target_link_libraries(tc_integration_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        endif()
        # ReaperTest includes the internal src/Reaper.hpp (detail::start_ryuk, session_id);
        # tests/support holds helpers shared with the unit tree (TestEnv.hpp).
        target_include_directories(tc_integration_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
        )
        tc_apply_warnings(tc_integration_tests)
        if(WIN32)
            target_link_libraries(tc_integration_tests PRIVATE ws2_32 mswsock)
        endif()
        if(MSVC)
            target_compile_definitions(tc_integration_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
        endif()
        add_test(NAME integration COMMAND tc_integration_tests)
        set_tests_properties(integration PROPERTIES LABELS "integration")
    endif()
endif()

# ===== Install =====
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS testcontainers
    EXPORT testcontainersTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# CMake package config for NON-Conan consumers: after `cmake --install`,
# `find_package(testcontainers)` imports testcontainers::testcontainers.
# (Conan consumers never see these files — the recipe's package() deletes
# them because CMakeDeps generates its own.)
install(EXPORT testcontainersTargets
    NAMESPACE testcontainers::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/testcontainers
)
configure_package_config_file(cmake/testcontainersConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/testcontainersConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/testcontainers
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/testcontainersConfigVersion.cmake
    COMPATIBILITY SameMinorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/testcontainersConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/testcontainersConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/testcontainers
)
