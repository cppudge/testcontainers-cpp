# Project conventions

Living document of conventions for testcontainers-cpp. Keep it short; add a rule
only when it is actually being followed.

## Layout & dependencies
- Public API headers live in `include/testcontainers/`; implementation and internal
  headers in `src/`. **Public headers must not include Boost/Asio/Beast** (or any
  other third-party) types — keep those strictly in `src/`.
- Internal, unit-testable mapping/parse logic goes in `src/docker/*` (e.g.
  `ApiMapping`, `LogDemux`) so it can be tested without a Docker daemon.

## Tests
- Unit tests (no Docker) live in `tests/unit/`; integration tests (real daemon,
  `GTEST_SKIP` when unavailable) in `tests/integration/`. Register both with CTest
  and the labels `unit` / `integration`.
- **Every test file carries a "test index" comment right after the include block:**
  a list of each `Suite.Name` in the file, each with a one-line, single-sentence
  description of what it checks (no line wrapping). It is the file's table of
  contents — keep it in sync when adding, renaming, or removing tests.

  Example:

  ```cpp
  #include "docker/LogDemux.hpp"

  // Tests in this file:
  //   LogDemux.SingleStdoutFrame - a lone stdout frame decodes to one frame with the stdout kind and its payload.
  //   LogDemux.DemuxAllIgnoresStdin - demux_all drops stdin frames and keeps only stdout/stderr.

  using testcontainers::docker::demux_all;
  ```

## Backlog
- Known limitations and deferred work are tracked in [`TODO.md`](../TODO.md), not
  scattered through code comments. When you spot tech debt during review, add it there.
