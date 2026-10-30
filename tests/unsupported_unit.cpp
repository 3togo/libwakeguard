// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "check.hpp"
#include <cstring>
#include <wakeguard/wakeguard.hpp>
int main() {
    auto options = wg_default_options();
    options.allow_suspend_block = 1;
    for (auto backend : {WG_BACKEND_AUTO, WG_BACKEND_NATIVE, WG_BACKEND_PORTAL, WG_BACKEND_LOGIND,
                         WG_BACKEND_CONSOLEKIT}) {
        options.backend = backend;
        wg_guard *out = nullptr;
        wg_error error{};
        CHECK(wg_acquire(&options, &out, &error) == WG_UNAVAILABLE);
        CHECK(out == nullptr && error.code == WG_UNAVAILABLE);
        CHECK(std::strcmp(error.message, "No backend compiled for this platform/build") == 0);
    }
    return test::result();
}
