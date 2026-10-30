// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "internal.hpp"
wg_guard *wg_platform_acquire(const wg_options &, std::string &detail) {
    detail = "No backend compiled for this platform/build";
    return nullptr;
}
