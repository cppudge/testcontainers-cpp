import os
from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMake, cmake_layout, CMakeDeps
from conan.tools.files import copy


class TestcontainersCppRecipe(ConanFile):
    name = "testcontainers-cpp"
    version = "0.1.0-alpha.0"
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

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "include/*",
        "src/*",
        "examples/*",
        "tests/*",
        "LICENSE*",
    )

    def requirements(self):
        self.requires("boost/1.91.0")
        self.requires("openssl/3.6.3")
        self.requires("nlohmann_json/3.12.0")
        self.requires("libarchive/3.8.7")

    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    def layout(self):
        cmake_layout(self)

    def build_requirements(self):
        if not self.conf.get("tools.build:skip_test", default=False):
            self.test_requires("gtest/1.14.0")

    def generate(self):
        CMakeDeps(self).generate()
        tc = CMakeToolchain(self)
        # We are already inside a conan-driven build; don't re-trigger the
        # cmake-conan provider from CMakeLists.txt.
        tc.cache_variables["SKIP_CONAN_PROVIDER_CMAKE"] = True
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE*", src=self.source_folder,
             dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        self.cpp_info.set_property("cmake_target_name", "testcontainers::testcontainers")
        self.cpp_info.libs = ["testcontainers"]
        if self.settings.os == "Windows":
            # Named-pipe transport pulls in Win32 networking libs.
            self.cpp_info.system_libs = ["ws2_32", "crypt32"]
