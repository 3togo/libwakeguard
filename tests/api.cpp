// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <wakeguard/wakeguard.hpp>
#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "failed: %s\n", #x);                                              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main() {
    static_assert(!std::is_copy_constructible<wakeguard::guard>::value);
    static_assert(std::is_nothrow_move_constructible<wakeguard::guard>::value);
    wg_error error{};
    auto options = wg_default_options();
    wg_guard *raw = nullptr;
    REQUIRE(wg_acquire(nullptr, &raw, &error) == WG_INVALID_ARGUMENT);
    REQUIRE(raw == nullptr && std::strlen(error.message) > 0);
    REQUIRE(wg_acquire(&options, nullptr, &error) == WG_INVALID_ARGUMENT);
    options.timeout_ms = 0;
    REQUIRE(wg_acquire(&options, &raw, nullptr) == WG_INVALID_ARGUMENT);
    options = wg_default_options();
    options.application = "";
    REQUIRE(wg_acquire(&options, &raw, &error) == WG_INVALID_ARGUMENT);
    options = wg_default_options();
    options.backend = WG_BACKEND_PORTAL;
    REQUIRE(wg_acquire(&options, &raw, &error) == WG_UNSUPPORTED);
    REQUIRE(std::strstr(error.message, "allow_suspend_block") != nullptr);
    REQUIRE(wg_check(nullptr, &error) == WG_INVALID_ARGUMENT);
    wg_release(nullptr);
    wakeguard::guard empty;
    REQUIRE(!empty);
    auto moved = std::move(empty);
    // Verify the documented empty state after moving ownership.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    REQUIRE(!moved && !empty);
    return 0;
}
