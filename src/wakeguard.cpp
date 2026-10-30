// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "internal.hpp"
#include <cstdio>
#include <exception>
#include <memory>

namespace {
wg_status report(wg_error *error, wg_status code, const char *message) {
    if (error) {
        error->code = code;
        std::snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return code;
}
} // namespace
extern "C" {
wg_options wg_default_options(void) {
    return {WG_BACKEND_AUTO, "libwakeguard", "Background work in progress", "", 3000, 0};
}
wg_status wg_acquire(const wg_options *options, wg_guard **out, wg_error *error) {
    if (out)
        *out = nullptr;
    if (!options || !out || !options->application || !options->reason || !options->window ||
        !*options->application || !*options->reason || options->timeout_ms < 1 ||
        options->timeout_ms > 60000 || options->backend < WG_BACKEND_AUTO ||
        options->backend > WG_BACKEND_CONSOLEKIT ||
        (options->allow_suspend_block != 0 && options->allow_suspend_block != 1))
        return report(error, WG_INVALID_ARGUMENT, "Invalid options or output pointer");
    if (options->backend == WG_BACKEND_PORTAL && !options->allow_suspend_block)
        return report(error, WG_UNSUPPORTED, "Portal Suspend requires allow_suspend_block=1");
    try {
        std::string detail;
        std::unique_ptr<wg_guard> guard(wg_platform_acquire(*options, detail));
        if (!guard)
            return report(error, WG_UNAVAILABLE, detail.c_str());
        report(error, WG_OK, "");
        *out = guard.release();
        return WG_OK;
    } catch (const std::exception &ex) {
        return report(error, WG_SYSTEM_ERROR, ex.what());
    } catch (...) {
        return report(error, WG_SYSTEM_ERROR, "Unexpected acquisition failure");
    }
}
void wg_release(wg_guard *guard) { delete guard; }
wg_backend wg_get_backend(const wg_guard *guard) {
    return guard ? guard->backend : WG_BACKEND_AUTO;
}
wg_scope wg_get_scope(const wg_guard *guard) { return guard ? guard->scope : WG_SCOPE_IDLE_SYSTEM; }
wg_status wg_check(const wg_guard *guard, wg_error *error) {
    if (!guard)
        return report(error, WG_INVALID_ARGUMENT, "Guard is NULL");
    try {
        std::string detail;
        const auto status = guard->check(detail);
        return report(error, status, detail.c_str());
    } catch (const std::exception &ex) {
        return report(error, WG_SYSTEM_ERROR, ex.what());
    } catch (...) {
        return report(error, WG_SYSTEM_ERROR, "Unexpected check failure");
    }
}
const char *wg_backend_name(wg_backend backend) {
    switch (backend) {
    case WG_BACKEND_AUTO:
        return "auto";
    case WG_BACKEND_NATIVE:
        return "native";
    case WG_BACKEND_PORTAL:
        return "portal";
    case WG_BACKEND_LOGIND:
        return "logind";
    case WG_BACKEND_CONSOLEKIT:
        return "consolekit";
    }
    return "unknown";
}
}
