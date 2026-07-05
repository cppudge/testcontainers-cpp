# ConanCenter recipe staging

This tree mirrors the layout of [conan-center-index](https://github.com/conan-io/conan-center-index)
(`recipes/testcontainers-cpp/...`) so a submission is a verbatim copy into a CCI fork.
It intentionally differs from the in-repo `/conanfile.py`, which exists for local builds:

| | in-repo recipe | CCI recipe (here) |
|---|---|---|
| sources | `exports_sources` (the working tree) | release tarball pinned in `conandata.yml` (sha256) |
| version | parsed from `TC_VERSION_FULL` | injected by CCI from `config.yml` (`--version` locally) |
| dependency options | boost header-only, libarchive trimmed | ecosystem defaults, nothing forced |
| tests | runs the unit suite unless `tools.build:skip_test` | never builds or runs tests |
| metadata | `url` = this repo | `url` = conan-center-index, `homepage` = this repo |

Verify locally (the tarball must be published — tag first):

    conan create packaging/conan-center/recipes/testcontainers-cpp/all \
        --version=0.1.0-alpha.1 --build=missing -s compiler.cppstd=20

Releasing a new version: bump `TC_VERSION_FULL`, tag `v<version>`, publish the GitHub
Release, then add the new version + tarball sha256 to `config.yml` / `conandata.yml`
(CCI accepts only the latest version in the initial submission).

Submission: fork conan-center-index, copy `recipes/testcontainers-cpp/` in, open a PR;
their CI builds ~30 configurations (Linux gcc/clang, Windows msvc, macOS apple-clang,
static/shared, multiple cppstds) and community review follows.
