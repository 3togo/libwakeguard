# Initial validation — 2026-10-30 UTC

Executed locally on Linux with GCC 15.3.0 and GIO 2.90.0:

| Evidence | Result | What it establishes |
| --- | --- | --- |
| Debug static build; CTest | 3 suites pass | C/C++ API checks and 16 private-bus protocol cases |
| Release shared build with `-Wall -Wextra -Wpedantic -Werror`; CTest | 3 suites pass | Shared exports, release behaviour and protocol checks |
| Installed static and shared packages, separate C and C++ CMake consumers | Both consumers pass for both packages | Installed headers, exported targets and link dependencies |
| Unix build with D-Bus disabled | Build and 2 API suites pass | Optional-dependency build path |
| AddressSanitizer + UndefinedBehaviorSanitizer | 3 suites pass | No reported memory/UB defects on exercised Linux paths |
| clang-tidy-18, project headers and compiled Linux sources/tests/example | No project diagnostics | Selected analyzer, bugprone, performance and portability checks |
| clang-format-18 dry run; Git diff whitespace check | Pass | Formatting and whitespace |
| Shared-library symbol inspection | Only 7 intended `wg_*` functions exported | C API visibility |

The host injects a preloaded library which initially prevented the ASan runtime
from starting first. Sanitizer tests were rerun successfully with `LD_PRELOAD`
unset **for that test command only**. No host setting was changed. Compiler/header
warnings suppressed by clang-tidy's system-header filter are not counted as
project diagnostics; narrow source suppressions document C ABI enum width,
GDBus flag combinations, and intentional moved-from-state assertions.

D-Bus tests cover actual calls through the public C++ wrapper/C API into the
production Unix backend. They use mock services on an isolated bus, not the host
power manager. They verify FD index 1 rather than assuming index 0, independent
ownership and crash cleanup, denial details, invalid FD replies, total timeouts,
missing/replaced service owners, strict fallback, portal scope opt-in, early
Response signals, refusals, absent confirmation, and connection cleanup.

Windows/macOS source and CI jobs exist, including native lifetime tests, but
were not executed locally or remotely. BSD compilation and actual platform
power-policy behaviour remain unvalidated. No physical suspend/display/lock,
Flatpak/AppImage, desktop-session or system-service validation has been performed.
Those gaps must be closed before claiming support on the corresponding targets.
The library is not yet integrated into aMule or published to GitHub.
