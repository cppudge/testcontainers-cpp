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
        --version=0.1.0 --build=missing -s compiler.cppstd=20

Releasing a new version: bump `TC_VERSION_FULL`, tag `v<version>`, publish the GitHub
Release, then add the version key to BOTH `config.yml` and `conandata.yml`, and the
tarball's sha256 to `conandata.yml` (CCI accepts only the latest version in the initial
submission).

Submission: fork conan-center-index, copy `recipes/testcontainers-cpp/` in (copy the
GIT-TRACKED files only — a naive `cp -r` would drag `test_package/build/` litter into
the PR), reference a package-request issue (`fixes #N`), open the PR titled
`testcontainers-cpp/<version>: new recipe`; their CI builds ~30 configurations and two
maintainer approvals are required.

The recipe is pinned to the stable `v0.1.0` tag (CCI's version model expects a released
tag; semver prereleases get pushed back on and are invisible to consumers' version
ranges by default — the earlier `0.1.0-alpha.1` pin was replaced on 2026-07-10).
