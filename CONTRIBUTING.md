# Contributing

Thanks for your interest in testcontainers-cpp. This is a solo-maintained project: the
maintainer pushes to `main` directly, and external contributions arrive as pull requests
from forks. Small, focused PRs are the easiest to review and land.

For anything non-trivial, please open an issue first so the approach can be agreed before
you write much code.

## Development setup

Requirements (the [README](README.md#requirements) has the full matrix): a C++20 compiler,
**CMake >= 3.21**, **Conan >= 2**, and **Ninja**. Dependencies are managed by Conan 2 and
pulled in automatically during CMake configure via the vendored cmake-conan provider — no
manual `conan profile detect` needed. By default the build uses a project-local Conan home
at `./.conan2`.

Configure, build, and test with the presets from `CMakePresets.json`:

```sh
cmake --preset ninja            # ninja-debug for a Debug build
cmake --build --preset ninja
ctest --preset ninja            # ctest --preset ninja-unit -> unit tests only
```

On Windows the Ninja preset needs `cl.exe` on `PATH`: run from an *"x64 Native Tools
Command Prompt for VS 2022"*, or use VS Code with the CMake Tools + clangd extensions,
which apply the developer environment for you. (A `msvc` Visual Studio preset also exists
as an IDE fallback.)

## Running the tests

Two suites:

- **Unit** (`tc_unit_tests`, CTest label `unit`) — no Docker required.
- **Integration** (`tc_integration_tests`, CTest label `integration`) — needs a reachable
  Docker daemon; individual tests `GTEST_SKIP` when the daemon or a required capability is
  unavailable, so the suite stays green (with skips) even without Docker.

Both engine modes are exercised: a Linux daemon over the unix socket / named pipe, and real
**Windows containers** on a Windows daemon. Run the suite against whichever engine mode your
change touches, and state which one in the PR.

## Format and lint gates

CI enforces formatting and linting with **pinned** tool versions, so match them locally to
avoid churn. `.clang-format` and `.clang-tidy` are the single source of truth (clangd
applies the same sets in-editor).

Install the pinned tools:

```sh
pip install clang-format==22.1.5
pip install clang-tidy==18.1.8
```

Check formatting (the tree must be byte-identical to what the pinned clang-format produces):

```sh
git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror
```

Run clang-tidy over the compile database (configure first so `build/ninja` exists):

```sh
git ls-files -z 'src/*.cpp' 'tests/*.cpp' 'examples/*.cpp' \
  | xargs -0 -n 2 clang-tidy -p build/ninja --quiet --warnings-as-errors='*'
```

CI also builds with **`-DTC_WERROR=ON`**, which turns `-Wall -Wextra` (gcc/clang) and `/W4`
(MSVC) into errors. The option defaults **OFF** so a new compiler's warnings never break
users; configure with `-DTC_WERROR=ON` locally to catch them before CI does.

## Conventions

See [`docs/conventions.md`](docs/conventions.md). Two that reviewers check on every PR:

- **Test-index comment.** Every test file carries a "Tests in this file" comment right after
  the include block: one line per `Suite.Name` with a single-sentence description, no line
  wrapping. Keep it in sync when you add, rename, or remove a test.
- **Backlog, not code comments.** Known limitations and deferred work go in
  [`docs/TODO.md`](docs/TODO.md), not scattered `// TODO` comments. If your change adds or
  alters an accepted limit, record it there (and in `docs/feature-notes.md` if it is a
  per-feature limit).

Public API headers must not include Boost/Asio/Beast (or other third-party) types — keep
those in `src/`. The full layout rules are in `docs/conventions.md`.

## Pull requests

- **Keep the diff focused** — one logical change per PR; unrelated cleanups belong in their
  own PR.
- **CI must be green.** Opening a PR from a fork runs the full build + test matrix (Linux
  and Windows engine modes) plus the format, tidy, and sanitizer gates. Fix any reds before
  requesting review.
- Fill in the pull request template: what/why, how you verified it (which suites, which
  engine mode), and the checklist.

## License

By contributing, you agree that your contributions are dual-licensed under MIT OR
Apache-2.0, matching the project. Specifically: unless you explicitly state otherwise, any
contribution intentionally submitted for inclusion in this work by you, as defined in the
Apache-2.0 license, shall be dual licensed as above, without any additional terms or
conditions.
