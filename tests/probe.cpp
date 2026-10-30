// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include <cstdlib>
#include <iostream>
#include <string>
#include <wakeguard/wakeguard.hpp>
int main(int argc, char **argv) {
    if (argc != 4)
        return 2;
    auto options = wg_default_options();
    options.application = "test-application";
    options.reason = "Transferring test files";
    options.backend = static_cast<wg_backend>(std::atoi(argv[1]));
    options.allow_suspend_block = std::atoi(argv[2]);
    options.timeout_ms = static_cast<unsigned int>(std::atoi(argv[3]));
    try {
        wakeguard::guard original(options);
        auto guard = std::move(original);
        // Verify the documented empty state after moving ownership.
        // NOLINTNEXTLINE(bugprone-use-after-move)
        if (original)
            return 3;
        std::cout << "OK " << wg_backend_name(guard.backend()) << ' ' << guard.scope() << '\n'
                  << std::flush;
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "check") {
                wg_error error{};
                const auto status = guard.check(&error);
                std::cout << "CHECK " << status << ' ' << error.message << '\n' << std::flush;
            } else if (command == "release") {
                guard.reset();
                std::cout << "RELEASED" << '\n' << std::flush;
                break;
            }
        }
    } catch (const wakeguard::acquisition_error &error) {
        std::cout << "ERROR " << error.code() << ' ' << error.what() << '\n' << std::flush;
        return 1;
    }
}
