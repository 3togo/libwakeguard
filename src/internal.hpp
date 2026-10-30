// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#pragma once
#include <string>
#include <wakeguard/wakeguard.h>

struct wg_guard {
    wg_backend backend = WG_BACKEND_AUTO;
    wg_scope scope = WG_SCOPE_IDLE_SYSTEM;
    virtual ~wg_guard() = default;
    virtual wg_status check(std::string &detail) const = 0;
};
wg_guard *wg_platform_acquire(const wg_options &options, std::string &detail);
