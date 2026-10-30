# Testing libwakeguard

On Linux, install a C/C++ toolchain, CMake, pkg-config, GIO development files,
`python3-gi` and D-Bus tools. To require every available Linux suite:

```sh
cmake -S . -B build -DWAKEGUARD_BUILD_TESTS=ON \
  -DWAKEGUARD_REQUIRE_DBUS_TESTS=ON -DWAKEGUARD_TEST_PYTHON=/usr/bin/python3
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

| Suite | What it verifies |
| --- | --- |
| `api` | Real library's invalid-argument and C++ wrapper behaviour |
| `c_api` | C compilation/linking and error reporting |
| `core_unit` | Production C boundary with injected backend outcomes: all rejected option fields, timeout boundaries, useful/truncated diagnostics, success clearing stale errors, standard/non-standard exceptions, status propagation and C++ ownership |
| `unsupported_unit` | Production no-backend implementation reports failure, never silent success |
| `windows_unit` | Production Windows control flow against minimal SDK doubles: UTF-8 conversion errors, creation/setup errors, reason/flags, handle lifetime, cross-thread release and cleanup even when clear fails |
| `macos_unit` | Production macOS control flow against minimal SDK doubles: CFString errors/lifetime, assertion errors, no ownership of failed output IDs, reason/type/level, independent assertions and cross-thread release |
| `dbus_integration` | Production Unix backend through the public wrapper, against private-bus services; 34 named tests with additional parameterized scenarios |
| `native_lifetime` | Windows/macOS only: acquisition, independent owners and cross-thread release against the real native SDK and OS |

Linux with GIO runs seven suites. Linux without GIO runs six; Windows/macOS run
five, including native lifetime. SDK doubles are compiled only into separate
Unix test executables. They are not installed, linked into the library or used
by real native builds. Their results do not verify native SDK declarations,
Unicode conversion implementation, ABI or actual power policy.

D-Bus tests verify idle-vs-suspend flags, FD index handling and lifetime, explicit
backend selection/fallback, refusals, malformed replies/signals, total acquisition
and authentication deadlines, connection cleanup, a service activated by D-Bus,
service owner replacement/disappearance, bus disconnection without an application
main loop, Unicode metadata, same-process replacement/reset and eight concurrent
guards in one process. A deliberate SIGKILL test checks crash cleanup. Other
clients exit normally and their exit status is checked, so destructor failures
and sanitizer errors cannot be hidden by forced teardown.

Linux unit and D-Bus tests never acquire host inhibitors, install host services
or change power settings. Native lifetime tests briefly acquire real inhibitors.
The activation helper is registered only inside a temporary private session bus.

## Other configurations

```sh
cmake -S . -B build-shared -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release \
  -DWAKEGUARD_BUILD_TESTS=ON -DWAKEGUARD_REQUIRE_DBUS_TESTS=ON
cmake --build build-shared --parallel
ctest --test-dir build-shared --output-on-failure

cmake -S . -B build-stub -DWAKEGUARD_WITH_DBUS=OFF -DWAKEGUARD_BUILD_TESTS=ON
cmake --build build-stub --parallel
ctest --test-dir build-stub --output-on-failure

cmake -S . -B build-sanitize -DWAKEGUARD_BUILD_TESTS=ON \
  -DWAKEGUARD_REQUIRE_DBUS_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

`python3 tests/coverage.py` builds GCC/gcov instrumentation in `build-coverage`,
resets its data, runs all Linux suites and writes `build-coverage/coverage-summary.json`.
It merges line hits across test executables that compile the same production
sources. Coverage is line coverage, not branch or requirement coverage. It rejects
gcov profile errors instead of publishing potentially incorrect percentages.

The development host injects a library through `LD_PRELOAD` that conflicts with
sanitizer/coverage runtimes. On that host only, run these test/coverage commands
with `env -u LD_PRELOAD` so the host library does not precede the instrumented
runtime. This does not change any persistent host setting. Normal CI needs no
such workaround.

Installed packages are checked independently by configuring `tests/install-consumer`
with `-DCMAKE_PREFIX_PATH=/path/to/installation`. CI does this for both static and
shared libraries on each native platform.

The test results cannot establish whether a particular desktop honours an idle
inhibitor, or whether an OS overrides requests due to manual suspend, battery or
administrator policy. Physical/platform validation remains separate.
