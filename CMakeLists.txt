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
    set(CMAKE_PROJECT_TOP_LEVEL_INCLUDES
        "${CMAKE_SOURCE_DIR}/cmake/cmake-conan/conan_provider.cmake")
endif()

# ===== Project =====
cmake_minimum_required(VERSION 3.20)
project(testcontainers VERSION 0.1.0 LANGUAGES CXX)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(TC_BUILD_EXAMPLES "Build examples" ON)

if(BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
endif()

# ===== Dependencies (provided by Conan / CMakeDeps) =====
find_package(Boost REQUIRED)            # header-only Beast + Asio
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(LibArchive REQUIRED)

# ===== Library =====
add_library(testcontainers
    src/version.cpp
    src/docker/DockerHost.cpp
    src/docker/Transport.cpp
    src/docker/ApiMapping.cpp
    src/docker/Auth.cpp
    src/docker/LogDemux.cpp
    src/docker/Tar.cpp
    src/docker/DockerClient.cpp
    src/WaitStrategies.cpp
    src/GenericImage.cpp
    src/GenericBuildableImage.cpp
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

target_include_directories(testcontainers
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src   # internal headers, e.g. "docker/Transport.hpp"
)
target_compile_features(testcontainers PUBLIC cxx_std_20)

# Third-party deps are implementation details: keep them PRIVATE so the public
# headers stay free of Boost/OpenSSL/json/libarchive types.
target_link_libraries(testcontainers PRIVATE
    boost::boost
    nlohmann_json::nlohmann_json
    OpenSSL::SSL
    OpenSSL::Crypto
    LibArchive::LibArchive
)

if(WIN32)
    # Asio named-pipe / TCP transport.
    target_link_libraries(testcontainers PRIVATE ws2_32 mswsock crypt32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(testcontainers PRIVATE Threads::Threads)
endif()

# ===== Examples =====
if(TC_BUILD_EXAMPLES)
    add_executable(tc_smoke examples/smoke.cpp)
    target_link_libraries(tc_smoke PRIVATE testcontainers::testcontainers)

    add_executable(tc_ping examples/ping.cpp)
    target_link_libraries(tc_ping PRIVATE testcontainers::testcontainers)
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
        tests/unit/ApiMappingTest.cpp
        tests/unit/AuthTest.cpp
        tests/unit/LogDemuxTest.cpp
        tests/unit/StreamReadTest.cpp
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
        # ITransport (allowed in a test translation unit — not a public header).
        boost::boost
        GTest::gtest
        GTest::gtest_main
    )
    # ApiMappingTest / AuthTest / TarTest include internal headers under src/.
    target_include_directories(tc_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME unit COMMAND tc_unit_tests)
    set_tests_properties(unit PROPERTIES LABELS "unit")

    # Integration tests — require a reachable Docker daemon (skipped otherwise).
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
        tests/integration/WindowsContainerTest.cpp
        tests/integration/ComposeTest.cpp
        tests/integration/TlsTransportTest.cpp
    )
    target_link_libraries(tc_integration_tests PRIVATE
        testcontainers::testcontainers
        # RedisMvpTest / ReaperTest open raw TCP sockets with Boost.Asio (allowed
        # in a test translation unit — it is not a public header).
        boost::boost
        # ReaperTest parses raw inspect JSON to read back a container's labels.
        nlohmann_json::nlohmann_json
        # TlsTransportTest runs an in-process Boost.Asio TLS echo server and
        # generates a self-signed cert/key via the OpenSSL C API.
        OpenSSL::SSL
        OpenSSL::Crypto
        GTest::gtest
        GTest::gtest_main
    )
    # ReaperTest includes the internal src/Reaper.hpp (detail::start_ryuk, session_id).
    target_include_directories(tc_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(WIN32)
        target_link_libraries(tc_integration_tests PRIVATE ws2_32 mswsock)
    endif()
    add_test(NAME integration COMMAND tc_integration_tests)
    set_tests_properties(integration PROPERTIES LABELS "integration")
endif()

# ===== Install =====
include(GNUInstallDirs)
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
