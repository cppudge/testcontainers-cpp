import os
import re
from conan import ConanFile
from conan.errors import ConanException, ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.files import copy, load, rmdir
from conan.tools.scm import Version


class TestcontainersCppRecipe(ConanFile):
    name = "testcontainers-cpp"
    package_type = "library"

    # Metadata
    license = "MIT OR Apache-2.0"
    author = "Petr Slaviagin <disident47@gmail.com>"
    url = "https://github.com/cppudge/testcontainers-cpp"
    description = "Native Testcontainers for C++ (Docker integration testing, no Rust)"
    topics = ("testcontainers", "testing", "containers", "docker", "integration-testing")

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False]}
    # Conan-managed shared/fPIC boilerplate (del fPIC on Windows / when shared).
    implements = ["auto_shared_fpic"]
    default_options = {
        "shared": False,
        "fPIC": True,
        # Beast + Asio + System are header-only — don't compile Boost at all.
        "boost/*:header_only": True,
        # libarchive only needed for tar; trim every other format/codec.
        # with_iconv pulls libiconv, which has no msvc-194 prebuilt binary and
        # fails to build from source on Windows (rc.exe/windres flag mismatch).
        "libarchive/*:with_iconv": False,
        "libarchive/*:with_zlib": True,
        "libarchive/*:with_bzip2": False,
        "libarchive/*:with_lzma": False,
        "libarchive/*:with_zstd": False,
        "libarchive/*:with_lz4": False,
        "libarchive/*:with_openssl": False,
        "libarchive/*:with_libxml2": False,
        "libarchive/*:with_expat": False,
    }

    # No "examples/*": generate() pins TC_BUILD_EXAMPLES=OFF, so a package
    # build never compiles them. Tests ARE exported — build() runs the unit
    # suite unless tools.build:skip_test says otherwise.
    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "src/*",
        "tests/*",
        "LICENSE*",
    )

    def requirements(self):
        self.requires("boost/1.91.0")
        self.requires("openssl/3.6.3")
        self.requires("nlohmann_json/3.12.0")
        self.requires("libarchive/3.8.7")
        # SSH client for the host-port-exposure sidecar tunnel (remote port
        # forwarding through a `testcontainers/sshd` container).
        self.requires("libssh2/1.11.1")

    def set_version(self):
        # Single source of truth: TC_VERSION_FULL in CMakeLists.txt.
        cml = load(self, os.path.join(self.recipe_folder, "CMakeLists.txt"))
        match = re.search(r'set\s*\(\s*TC_VERSION_FULL\s+"([^"]+)"', cml)
        if not match:
            raise ConanException("could not parse TC_VERSION_FULL from CMakeLists.txt")
        self.version = match.group(1)

    def validate(self):
        # The public headers use C++20 (concepts-free, but <=17 won't parse
        # them); require the profile to say so rather than fail mid-build.
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, 20)
        # Keep in sync with packaging/conan-center/.../conanfile.py.
        minimum_versions = {
            "gcc": "12",
            "clang": "15",
            "apple-clang": "15",
            "msvc": "193",
        }
        minimum = minimum_versions.get(str(self.settings.compiler))
        if minimum and Version(self.settings.compiler.version) < minimum:
            raise ConanInvalidConfiguration(
                f"{self.name} requires C++20; {self.settings.compiler} "
                f"{self.settings.compiler.version} < {minimum} is not supported")
        if self.options.get_safe("shared") and self.settings.os == "Windows":
            # The sources carry no dllexport/visibility macros yet, so a
            # Windows DLL would export nothing and consumers could not link.
            raise ConanInvalidConfiguration(
                "shared builds are not supported on Windows (no symbol export macros yet)")

    def layout(self):
        cmake_layout(self)

    def build_requirements(self):
        if not self.conf.get("tools.build:skip_test", default=False, check_type=bool):
            self.test_requires("gtest/1.14.0")

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        # We drive CMake via CMakePresets.json + the cmake-conan provider, so do
        # NOT let Conan write/append a CMakeUserPresets.json: with several build
        # dirs (ninja, ninja-debug, msvc) it would accumulate one "conan-default"
        # include per dir and CMake would reject the duplicate preset.
        tc.user_presets_path = False
        # We are already inside a conan-driven build; don't re-trigger the
        # cmake-conan provider from CMakeLists.txt.
        tc.cache_variables["SKIP_CONAN_PROVIDER_CMAKE"] = True
        # A package build ships the library only — examples are for the repo.
        tc.cache_variables["TC_BUILD_EXAMPLES"] = False
        # Pin the GNUInstallDirs layout: on non-Debian 64-bit Linux it would
        # pick lib64, and then the package()'s rmdir("lib/cmake") and the
        # default cpp_info.libdirs=["lib"] would both point past the real
        # install location.
        tc.cache_variables["CMAKE_INSTALL_LIBDIR"] = "lib"
        if self.conf.get("tools.build:skip_test", default=False, check_type=bool):
            # CMakeToolchain sets this itself on current Conan versions;
            # keep it explicit so the behavior doesn't hang on that detail.
            tc.cache_variables["BUILD_TESTING"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()
        if not self.conf.get("tools.build:skip_test", default=False, check_type=bool):
            # Unit tests only: the integration suite wants a Docker daemon,
            # which a package build must not assume.
            cmake.ctest(cli_args=["--output-on-failure", "-L", "unit"])

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE*", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))
        # The install tree carries CMake config files for NON-Conan consumers;
        # Conan consumers get configs generated by CMakeDeps, and a stale
        # installed copy alongside them would shadow the generated ones.
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        # Match the non-Conan install: find_package(testcontainers) and the
        # testcontainers::testcontainers target (the package NAME stays
        # testcontainers-cpp for the reference on a remote).
        self.cpp_info.set_property("cmake_file_name", "testcontainers")
        self.cpp_info.set_property("cmake_target_name", "testcontainers::testcontainers")
        self.cpp_info.libs = ["testcontainers"]
        # Scope the transitive link lines to what the static lib really needs:
        # only Boost's HEADERS component (Beast/Asio/System are header-only).
        # This recipe pins boost header-only so it changes little here, but it
        # must match the ConanCenter recipe, where boost arrives fully built.
        self.cpp_info.requires = [
            "boost::headers",
            "openssl::ssl",
            "openssl::crypto",
            "nlohmann_json::nlohmann_json",
            "libarchive::libarchive",
            "libssh2::libssh2",
        ]
        if self.settings.os == "Windows":
            # Named-pipe / socket transport pulls in Win32 networking libs —
            # keep in sync with the target_link_libraries in CMakeLists.txt.
            self.cpp_info.system_libs = ["ws2_32", "mswsock", "crypt32"]
        else:
            # Threads::Threads is PRIVATE in CMake, but a static consumer
            # still links it transitively.
            self.cpp_info.system_libs = ["pthread"]
