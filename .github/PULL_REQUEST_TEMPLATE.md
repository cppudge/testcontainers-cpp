## What & why

<!-- What does this change do, and why? Link any related issue. -->

## How verified

<!-- Which suites did you run, and in which engine mode? -->

- [ ] Unit (`tc_unit_tests`)
- [ ] Integration (`tc_integration_tests`) — engine mode: Linux / Windows
- Other:

## Checklist

- [ ] `clang-format` clean (pinned 22.1.5) — `... | xargs -0 clang-format --dry-run --Werror`
- [ ] `clang-tidy` clean (pinned 18.1.8) — `--warnings-as-errors='*'`
- [ ] Builds with `-DTC_WERROR=ON` (no `-Wall -Wextra` / `/W4` warnings)
- [ ] Test-index ("Tests in this file") comments updated for any touched test file
- [ ] `docs/TODO.md` / `docs/feature-notes.md` updated if an accepted limit changed
- [ ] Diff is focused (no unrelated changes)
