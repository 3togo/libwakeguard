# Version 0.2 validation — 2026-10-30 UTC

Local environment: Linux, GCC 15.3.0, GIO 2.90.0. See [tests/README.md](tests/README.md)
for reproduction commands and the distinction between unit, protocol and native tests.

| Evidence | Result | What it establishes |
| --- | --- | --- |
| Debug static build | 8/8 CTest suites pass | Public C/C++ API, core/native/unsupported unit tests and 36 D-Bus integration tests |
| Release shared build with `-Wall -Wextra -Wpedantic -Werror` | 8/8 suites pass | Shared exports and Release execution of those paths |
| Unix build with D-Bus disabled | 7/7 suites pass | Optional-dependency path, including real unsupported-backend failure |
| AddressSanitizer + UndefinedBehaviorSanitizer | 8/8 suites pass | No reported memory/UB defects in exercised paths, including normal destructor cleanup |
| Core/controller/Windows/macOS/unsupported unit executables | 307 assertions, zero failures | Fault injection, diagnostic propagation and independent ownership |
| Installed static/shared packages and separate C/C++ CMake consumers | Both consumers pass for both packages | Installed headers, targets and link dependencies |
| clang-tidy-18; clang-format-18; Git whitespace checks | Pass | No project diagnostics under configured analysis checks; formatting |

## Line coverage

GCC/gcov instrumentation, combining production source compiled into the real
Unix library and the independent fault-injection executables:

| Production file | Executed lines | Percentage |
| --- | --- | --- |
| `src/wakeguard.cpp` | 58/58 | 100% |
| `src/windows.cpp` | 38/38 | 100% (SDK doubles) |
| `src/macos.cpp` | 24/24 | 100% (SDK doubles) |
| `src/unsupported.cpp` | 3/3 | 100% |
| `src/unix.cpp` | 195/206 | 94.7% |
| `src/controller.cpp` | 105/127 | 82.7% |

This is line coverage, not branch coverage or evidence that all requirements are
satisfied. The remaining Unix paths include bus-address discovery failure,
health-check timeout, the internal invalid-backend guard and FD-duplication failure.
Native SDK doubles do not validate Apple/Windows ABI, the OS implementation of
Unicode conversion, or native power-policy effects. Source-line counts depend
on compiler instrumentation.

## Asynchronous policy validation

The controller unit test also passed under ThreadSanitizer. It checks option
snapshots, repeated tick coalescing, disable/enable generations, late results,
backoff and failure-detail propagation, health loss/recovery and joined shutdown.
Private-bus tests verify explicit `sleep:idle` requests and that a silent portal
cannot exhaust the entire budget before logind is attempted. Allocation-failure
fallbacks in the worker remain reviewed but unexercised.

## Correction found by tests

A new macOS failure-path test writes a non-owned assertion ID into a failed
call's output parameter. Previously cleanup attempted to release that ID.
The backend now transfers the assertion ID into its guard only after success.
The regression test fails before the correction and passes afterwards.

## Environment and remaining coverage

The host injects a library through `LD_PRELOAD` which prevents ASan from starting
first and can invalidate gcov data. The instrumented suites were run with
`LD_PRELOAD` unset for those commands only; no host setting was changed. Project
analysis has narrowly documented suppressions for C ABI enum widths, GDBus flags,
a Windows SDK selection macro, intentional invalid-input fixtures and moved-from
ownership assertions.

GitHub Actions jobs for native Windows/macOS builds and native lifetime tests
were queued at the time of this local report. They are not recorded as passing.
The workflow also runs Linux static/shared tests, installed consumers, a no-D-Bus
build, sanitizers, static analysis, formatting and coverage. Check the published
commit's run for current status.

BSD compilation, physical suspend/display/lock behaviour, Flatpak/AppImage and
actual desktop/service deployments remain unvalidated. Tests use isolated D-Bus
services and do not change host power settings. The repository is public at
https://github.com/3togo/libwakeguard; aMule integration is maintained separately in the aMule repository.
