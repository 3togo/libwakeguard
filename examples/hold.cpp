// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include <iostream>
#include <string>
#include <wakeguard/wakeguard.hpp>
int main(int argc, char **argv) {
    auto options = wg_default_options();
    options.application = "wakeguard-hold";
    options.reason = "User requested background work";
    if (argc > 1 && std::string(argv[1]) == "--allow-suspend-block")
        options.allow_suspend_block = 1;
    try {
        wakeguard::guard guard(options);
        std::cout << "Acquired " << wg_backend_name(guard.backend())
                  << (guard.scope() == WG_SCOPE_SUSPEND ? " (suspend scope)"
                                                        : " (idle-system scope)")
                  << ". Press Enter to release.\n";
        std::string line;
        std::getline(std::cin, line);
    } catch (const wakeguard::acquisition_error &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
