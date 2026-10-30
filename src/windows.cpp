// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#ifndef _WIN32_WINNT
// Windows SDK selection macro, required to declare the Windows 7 power APIs.
// NOLINTNEXTLINE(bugprone-reserved-identifier)
#define _WIN32_WINNT 0x0601
#endif
#include "internal.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <memory>
#include <vector>
#include <windows.h>

namespace {
struct native_guard final : wg_guard {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool acquired = false;
    ~native_guard() override {
        if (handle != INVALID_HANDLE_VALUE) {
            if (acquired)
                PowerClearRequest(handle, PowerRequestSystemRequired);
            CloseHandle(handle);
        }
    }
    wg_status check(std::string &) const override { return WG_OK; }
};
std::string system_error(const char *operation) {
    const DWORD code = GetLastError();
    return std::string(operation) + " failed (Win32 error " + std::to_string(code) + ")";
}
} // namespace
wg_guard *wg_platform_acquire(const wg_options &options, std::string &detail) {
    if (options.backend != WG_BACKEND_AUTO && options.backend != WG_BACKEND_NATIVE) {
        detail = "Requested backend is unavailable on Windows";
        return nullptr;
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, options.reason, -1, nullptr, 0);
    if (!length) {
        detail = system_error("UTF-8 reason conversion");
        return nullptr;
    }
    std::vector<wchar_t> reason(static_cast<size_t>(length));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, options.reason, -1, reason.data(),
                             length)) {
        detail = system_error("UTF-8 reason conversion");
        return nullptr;
    }
    REASON_CONTEXT context{};
    context.Version = POWER_REQUEST_CONTEXT_VERSION;
    context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    context.Reason.SimpleReasonString = reason.data();
    auto guard = std::make_unique<native_guard>();
    guard->backend = WG_BACKEND_NATIVE;
    guard->handle = PowerCreateRequest(&context);
    if (guard->handle == INVALID_HANDLE_VALUE) {
        detail = system_error("PowerCreateRequest");
        return nullptr;
    }
    if (!PowerSetRequest(guard->handle, PowerRequestSystemRequired)) {
        detail = system_error("PowerSetRequest");
        return nullptr;
    }
    guard->acquired = true;
    return guard.release();
}
