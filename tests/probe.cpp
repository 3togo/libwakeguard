// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
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
    if (std::getenv("WG_TEST_INVALID_UTF8"))
        options.reason = "\xff";
    if (std::getenv("WG_TEST_UNICODE")) {
        options.application = "下載器";
        options.reason = "正在傳輸檔案";
        options.window = "x11:1234";
    }
    if (std::getenv("WG_TEST_PARALLEL")) {
        std::array<wg_status, 8> results{};
        std::vector<std::thread> workers;
        for (std::size_t i = 0; i < results.size(); ++i) {
            workers.emplace_back([&, i] {
                wg_guard *guard = nullptr;
                results[i] = wg_acquire(&options, &guard, nullptr);
                if (results[i] == WG_OK)
                    results[i] = wg_check(guard, nullptr);
                wg_release(guard);
            });
        }
        for (auto &worker : workers)
            worker.join();
        for (auto status : results)
            if (status != WG_OK)
                return 4;
        std::cout << "PARALLEL OK\n" << std::flush;
        return 0;
    }
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
            } else if (command == "replace") {
                guard = wakeguard::guard(options);
                std::cout << "REPLACED\n" << std::flush;
            } else if (command == "reset") {
                guard.reset();
                std::cout << "RESET\n" << std::flush;
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
