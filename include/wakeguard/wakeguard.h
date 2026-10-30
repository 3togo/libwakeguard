/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 libwakeguard contributors
 */
#ifndef WAKEGUARD_H
#define WAKEGUARD_H
#include <stddef.h>
#include <wakeguard/export.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct wg_guard wg_guard;
// C-compatible enums intentionally use the C ABI default width.
// NOLINTNEXTLINE(performance-enum-size)
typedef enum wg_backend {
    WG_BACKEND_AUTO = 0,
    WG_BACKEND_NATIVE,
    WG_BACKEND_PORTAL,
    WG_BACKEND_LOGIND,
    WG_BACKEND_CONSOLEKIT
} wg_backend;
// NOLINTNEXTLINE(performance-enum-size)
typedef enum wg_status {
    WG_OK = 0,
    WG_INVALID_ARGUMENT,
    WG_UNSUPPORTED,
    WG_UNAVAILABLE,
    WG_SYSTEM_ERROR,
    WG_LOST
} wg_status;
// NOLINTNEXTLINE(performance-enum-size)
typedef enum wg_scope { WG_SCOPE_IDLE_SYSTEM = 0, WG_SCOPE_SUSPEND } wg_scope;

/* Initialize with wg_default_options(). Strings are UTF-8 and are copied during
 * acquire. timeout_ms is a total Unix acquisition budget (1..60000 ms).
 * Native OS calls do not offer cancellation/deadlines.
 * allow_suspend_block=1 opts into portal Suspend and logind/ConsoleKit sleep:idle
 * locks, which may also affect explicit suspend. Zero keeps idle-only requests.
 * No backend requests screen/idle-session/lock inhibition.
 * window is an optional XDG portal window identifier, or an empty string.
 */
typedef struct wg_options {
    wg_backend backend;
    const char *application;
    const char *reason;
    const char *window;
    unsigned int timeout_ms;
    int allow_suspend_block;
} wg_options;
/* Caller-owned diagnostic storage, always NUL terminated by library calls. */
typedef struct wg_error {
    wg_status code;
    char message[1024];
} wg_error;

WG_EXPORT wg_options wg_default_options(void);
/* Blocking: run on your application's worker thread. No event loop required.
 * On failure *out is NULL; error may be NULL. Each success owns an independent
 * request. Successful acquisition means the service accepted a request, not a
 * guarantee that OS policy or every desktop idle manager will honour it.
 */
WG_EXPORT wg_status wg_acquire(const wg_options *options, wg_guard **out, wg_error *error);
/* NULL-safe. Releases only this guard. Do not use the pointer afterwards. */
WG_EXPORT void wg_release(wg_guard *guard);
WG_EXPORT wg_backend wg_get_backend(const wg_guard *guard);
WG_EXPORT wg_scope wg_get_scope(const wg_guard *guard);
/* A bounded Unix service-owner/connection check, not a power-policy check.
 * On WG_LOST, release and explicitly reacquire with your own retry/backoff.
 * Native backends cannot detect policy revocation and report WG_OK.
 */
WG_EXPORT wg_status wg_check(const wg_guard *guard, wg_error *error);
WG_EXPORT const char *wg_backend_name(wg_backend backend);
/* Independent guards may be used concurrently. Serialize access to a single
 * guard, including check/release. No library global error buffer is used.
 */
#ifdef __cplusplus
}
#endif
#endif
