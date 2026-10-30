// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include <iostream>
#include <thread>
#include <wakeguard/wakeguard.hpp>
int main() {
    auto options = wg_default_options();
    options.reason = "libwakeguard native lifetime test";
    try {
        wakeguard::guard first(options);
        wakeguard::guard second(options);
        if (first.backend() != WG_BACKEND_NATIVE || first.scope() != WG_SCOPE_IDLE_SYSTEM)
            return 1;
        // Handle/assertion ownership permits release on a different thread.
        std::thread worker([owned = std::move(first)]() mutable { owned.reset(); });
        worker.join();
        // Verify the documented empty state after moving ownership.
        // NOLINTNEXTLINE(bugprone-use-after-move)
        if (first || second.check() != WG_OK)
            return 1;
        second.reset();
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
