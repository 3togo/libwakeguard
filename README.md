# libwakeguard

A small, GUI-independent C++17 library for requesting that a computer remain
awake during background work. C API, move-only C++ wrapper, CMake package, MIT licence.
Version 0.2 is an initial implementation; the ABI is not yet stable.

The default contract is **prevent idle system sleep**. It does not request
screen, screen-lock, logout or shutdown inhibition. Explicit suspend, lid policy,
critical battery actions and administrator decisions remain OS policy.

## Backends and honest capabilities

| Backend | Mechanism | Requested scope | Runtime requirements |
| --- | --- | --- | --- |
| Windows | `PowerCreateRequest` / `PowerSetRequest(SystemRequired)` | Idle system sleep | Windows 7+ power-request API |
| macOS | IOKit `PreventUserIdleSystemSleep` assertion | Idle system sleep | macOS IOKit |
| logind-compatible | `org.freedesktop.login1.Manager.Inhibit("idle", ..., "block")` | Idle system sleep | System bus, compatible service and permission |
| ConsoleKit2 | `org.freedesktop.ConsoleKit.Manager.Inhibit("idle", ..., "block")` | Idle system sleep | System bus, ConsoleKit2 with inhibitor support |
| XDG desktop portal | `org.freedesktop.portal.Inhibit`, Suspend flag 4 | Suspend, potentially including explicit suspend | Session bus, implementing portal backend and permission; explicit caller opt-in |

Unix backend selection depends on services, not distro names or X11/Wayland.
Compatible elogind services can expose the same login1 API; no systemd library
is linked. Unix backends are compiled on Unix systems with GIO, including BSD,
but availability depends on installed services and policy. This is not a claim
of physical BSD validation.

With `WG_BACKEND_AUTO`, strict mode tries logind then ConsoleKit2. Setting
`allow_suspend_block = 1` first tries portal Suspend, then `sleep:idle` locks
through logind and ConsoleKit2. This is an explicit policy choice: it can also
affect user-started suspend. An explicit backend disables fallback. The returned
scope tells you what was requested. Native Windows/macOS backends remain idle-only.
No backend substitutes screensaver suppression or simulated user input.

**An accepted inhibitor is not proof that a desktop honours it.** For example,
a desktop's own automatic-suspend policy may not consult a logind idle lock.
Strict mode deliberately does not silently switch to blocking explicit suspend.
Flatpak applications that cannot reach the system bus need the portal option,
and therefore must opt into its broader scope. There is no claimed strict
idle-only Flatpak backend. Native/AppImage use the services visible on the host.
Services without a user session can use logind/ConsoleKit2 when policy permits;
absence of a session portal is normal.

Windows Modern Standby can terminate power requests on battery; manual sleep
can end them. Native `check()` cannot establish whether OS policy revoked a request.

## Build and use

```sh
cmake -S . -B build -DWAKEGUARD_BUILD_EXAMPLES=ON -DWAKEGUARD_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /your/prefix
```

Unix dependencies: C++17 toolchain, CMake 3.12+, pkg-config, GIO/GLib (`gio-unix-2.0`
2.40+), threads. Ubuntu/Debian: `libglib2.0-dev`. Windows and macOS use native SDKs
and do not need GLib. `-DWAKEGUARD_WITH_DBUS=OFF` builds a Unix library which
reports acquisition as unavailable. `-DBUILD_SHARED_LIBS=ON` builds a shared library.

Consumers can use `add_subdirectory()` or an installed package:

```cmake
find_package(wakeguard 0.2 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE wakeguard::wakeguard)
```

```cpp
#include <wakeguard/wakeguard.hpp>

auto options = wg_default_options();
options.application = "my-downloader";
options.reason = "Transferring files";
try {
    wakeguard::guard awake(options);
    // Perform background work. Scope exit releases this inhibitor.
} catch (const wakeguard::acquisition_error &error) {
    // Log error.what(); work can continue without an inhibitor.
}
```

C consumers initialize `wg_options` with `wg_default_options()`, call
`wg_acquire(&options, &guard, &error)`, then `wg_release(guard)` exactly once on
success. `wg_error` is caller-owned; no shared static diagnostic buffer exists.
`error` can be NULL. Output is NULL on acquisition failure. Strings must remain
valid throughout the acquisition call. The guard does not borrow caller strings.
See `include/wakeguard/wakeguard.h` for the complete contract.

## Lifetime, threading and recovery

Acquire and check are **blocking**. Dispatch them on an application worker
thread; do not call them on every GUI/download tick. Unix acquisition has one
total deadline (default 3 seconds) covering discovery, authentication, service
activation, fallback and portal confirmation. Each eligible backend gets a share
of the remaining budget so a silent portal cannot starve logind/ConsoleKit2.
Fallback never receives a fresh total budget. Native OS APIs do not
expose a comparable cancellation deadline. For an idempotent background state interface, use `wakeguard::controller`:

```cpp
auto options = wg_default_options();
options.application = "aMule";
options.reason = "Transferring files";
// For a desktop portal-first policy, explicitly permit broader suspend locks:
// options.allow_suspend_block = 1;
wakeguard::controller sleep(options);
sleep.set_required(true);  // Tick-safe; the worker acquires and checks the lock.
wakeguard::controller_status event;
if (sleep.take_event(event)) {
    // Log event.detail on this thread; state/backend/scope identify the result.
}
sleep.set_required(false); // Releases asynchronously; late results are discarded.
sleep.shutdown();          // Join and release before application shutdown.
```

The controller snapshots option strings, coalesces repeated desired-state updates,
checks service health and retries failures (default intervals 30 seconds each,
accepted range 1 millisecond through 24 hours).
A health-probe timeout retains an owned inhibitor; confirmed loss releases it.
A disable/enable generation change discards stale acquisitions. `take_event()`
delivers the latest transition, not a history of every intermediate state.
Serialize `shutdown()`/destruction; set_required and take_event may run concurrently.
Shutdown may wait for the acquisition budget or a native OS call. Direct guard
users may instead dispatch work on their own executor.

Every guard has independent ownership. Windows uses a handle rather than
thread-affine execution-state calls; macOS uses an assertion ID. Unix uses a
private bus connection and, for logind/ConsoleKit2, an owned file descriptor.
Connection loss cleans up portal requests, including ambiguous timed-out replies.
Release closes the FD immediately and starts asynchronous connection close; portal
cleanup completes after the bus/backend processes disconnection. It is not a
synchronous publication guarantee. Process exit/crash also releases kernel/bus
ownership. Do not carry guards across `fork()`; create them after forking.

Independent guards may be used concurrently. Serialize access to the same guard,
including release. A guard may move between threads. C++ `guard` is move-only.

Unix `check()` checks the bus connection and service owner (1 second budget).
A changed/missing owner returns `WG_LOST`; a timed-out check returns
`WG_UNAVAILABLE`. Release and reacquire after loss using application-controlled
backoff. This does not detect every policy change, a portal backend silently
revoking an individual request, or a desktop ignoring an inhibitor. There is no
automatic reconnect thread and no claim that native policy revocation is detected.

For aMule, keep the transfer/preference decision in the core and keep this library
independent of wxWidgets. Acquire on transitions into the desired state, release
on transitions out and shutdown, and retry failures with backoff. If desired
state changes while acquisition is pending, release the late result. Do not
reinterpret existing queue/upload policy as part of integration. Both `amule` and
`amuled` should use the same library; `amulegui` controls a different machine.
The library contains no aMule transfer policy; its adapter is maintained in the aMule repository.

## Validation

Protocol tests use a private D-Bus session with its address supplied as a mock
system bus. They never acquire host power inhibitors or change host settings.
Install `python3-gi` and `dbus` for these tests; inspect CTest output to confirm
that `dbus_integration` is registered (otherwise CMake reports why it is skipped).

The suite includes 36 private-bus tests plus C/C++ API and deterministic core,
Windows, macOS and unsupported-backend unit tests. Native SDK doubles exercise
production ownership/error paths on Linux; they do not establish native ABI or
power-policy correctness. Tests caught and fixed ownership of an output assertion
ID after a failed macOS acquisition. See [tests/README.md](tests/README.md) for
coverage, dependencies, sanitizer commands and repeatable line-coverage generation.
`-DWAKEGUARD_REQUIRE_DBUS_TESTS=ON` makes missing integration prerequisites fatal
instead of silently omitting that suite.

CI is configured for static/shared Linux, Windows and macOS builds, API checks,
native Windows/macOS acquisition/lifetime checks and a Unix build without D-Bus.
CI also includes sanitizers, clang-tidy-18, clang-format-18 and coverage.
Configured CI is not evidence that those jobs have run; see `VALIDATION.md` for
observed results. Local execution covers Linux builds and mocked native/protocol
interfaces. Actual idle-suspend,
manual-suspend, display blanking and lock behaviour still require physical tests
on Cinnamon, GNOME, KDE, Windows, macOS and intended BSD environments, plus
Flatpak, AppImage and headless/service deployments. No desktop/platform is marked
physically validated by a passing mock test.

## Licence and references

MIT; usable in GPL-2.0-or-later aMule with notices retained. The combined
application remains subject to aMule's GPL. See `LICENSE`, `NOTICE.md` and
`third_party/keepawake-rs.LICENSE`. Unix GIO/GLib dependencies retain their own LGPL
terms; they are not bundled here.

Behavioural reference: [keepawake-rs](https://github.com/segevfiner/keepawake-rs).
An independent C++ implementation, with no Rust dependency or incorporated Rust code.

Protocols: [Windows power requests](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-powersetrequest),
[Apple assertions](https://developer.apple.com/documentation/iokit/iopmassertioncreatewithname),
[logind](https://systemd.io/INHIBITOR_LOCKS/),
[ConsoleKit2 interface](https://github.com/ConsoleKit2/ConsoleKit2/blob/master/src/org.freedesktop.ConsoleKit.Manager.xml),
[XDG Inhibit](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Inhibit.html).
