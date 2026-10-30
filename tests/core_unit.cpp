// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "check.hpp"
#include "internal.hpp"
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <wakeguard/wakeguard.hpp>

namespace {
enum class behaviour : std::uint8_t { success, unavailable, standard_exception, unknown_exception };
behaviour acquire_behaviour = behaviour::success;
behaviour check_behaviour = behaviour::success;
int calls = 0;
int destroyed = 0;
std::string diagnostic;
wg_status check_status = WG_OK;
struct fake_guard final : wg_guard {
    ~fake_guard() override { ++destroyed; }
    wg_status check(std::string &detail) const override {
        if (check_behaviour == behaviour::standard_exception)
            throw std::runtime_error("check exception detail");
        if (check_behaviour == behaviour::unknown_exception)
            throw 42;
        detail = diagnostic;
        return check_status;
    }
};
void invalid_options() {
    const auto initial = wg_default_options();
    CHECK(initial.backend == WG_BACKEND_AUTO);
    CHECK(initial.timeout_ms == 3000);
    CHECK(initial.allow_suspend_block == 0);
    CHECK(initial.application && *initial.application);
    CHECK(initial.reason && *initial.reason);
    CHECK(initial.window && !*initial.window);
    const auto invalid = [](const wg_options *options, wg_status status = WG_INVALID_ARGUMENT) {
        wg_error error{};
        fake_guard sentinel;
        wg_guard *out = &sentinel;
        const auto before = calls;
        CHECK(wg_acquire(options, &out, &error) == status);
        CHECK(out == nullptr);
        CHECK(error.code == status);
        CHECK(error.message[0] != '\0');
        CHECK(calls == before);
        CHECK(wg_acquire(options, &out, nullptr) == status);
    };
    invalid(nullptr);
    wg_options options = initial;
    options.application = nullptr;
    invalid(&options);
    options = initial;
    options.application = "";
    invalid(&options);
    options = initial;
    options.reason = nullptr;
    invalid(&options);
    options = initial;
    options.reason = "";
    invalid(&options);
    options = initial;
    options.window = nullptr;
    invalid(&options);
    options = initial;
    options.timeout_ms = 0;
    invalid(&options);
    options = initial;
    options.timeout_ms = 60001;
    invalid(&options);
    options = initial;
    options.allow_suspend_block = -1;
    invalid(&options);
    options = initial;
    options.allow_suspend_block = 2;
    invalid(&options);
    // 5 is representable by this enum, but is not an API-defined backend.
    options = initial;
    // Deliberately exercise rejection of an undefined C API backend value.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    options.backend = static_cast<wg_backend>(5);
    invalid(&options);
    options = initial;
    options.backend = WG_BACKEND_PORTAL;
    invalid(&options, WG_UNSUPPORTED);
    wg_error error{};
    const auto before = calls;
    CHECK(wg_acquire(&initial, nullptr, &error) == WG_INVALID_ARGUMENT);
    CHECK(calls == before);
    CHECK(wg_get_backend(nullptr) == WG_BACKEND_AUTO);
    CHECK(wg_get_scope(nullptr) == WG_SCOPE_IDLE_SYSTEM);
    CHECK(wg_check(nullptr, &error) == WG_INVALID_ARGUMENT);
    CHECK(error.code == WG_INVALID_ARGUMENT);
    CHECK(wg_check(nullptr, nullptr) == WG_INVALID_ARGUMENT);
    wg_release(nullptr);
    const char *names[] = {"auto", "native", "portal", "logind", "consolekit", "unknown"};
    for (int i = 0; i < 6; ++i)
        CHECK(std::strcmp(wg_backend_name(static_cast<wg_backend>(i)), names[i]) == 0);
}
void failures_and_diagnostics() {
    auto options = wg_default_options();
    wg_guard *out = nullptr;
    wg_error error{};
    diagnostic = std::string(4096, 'x');
    acquire_behaviour = behaviour::unavailable;
    std::memset(&error, 0x7f, sizeof(error));
    CHECK(wg_acquire(&options, &out, &error) == WG_UNAVAILABLE);
    CHECK(out == nullptr && error.code == WG_UNAVAILABLE);
    CHECK(error.message[sizeof(error.message) - 1] == '\0');
    CHECK(std::strlen(error.message) == sizeof(error.message) - 1);
    for (const auto mode : {behaviour::standard_exception, behaviour::unknown_exception}) {
        acquire_behaviour = mode;
        CHECK(wg_acquire(&options, &out, &error) == WG_SYSTEM_ERROR);
        CHECK(out == nullptr && error.code == WG_SYSTEM_ERROR);
        CHECK(std::strstr(error.message, mode == behaviour::standard_exception
                                             ? "acquisition exception detail"
                                             : "Unexpected acquisition failure") != nullptr);
        CHECK(wg_acquire(&options, &out, nullptr) == WG_SYSTEM_ERROR);
    }
    try {
        wakeguard::guard guard(options);
        CHECK(false);
    } catch (const wakeguard::acquisition_error &exception) {
        CHECK(exception.code() == WG_SYSTEM_ERROR);
        CHECK(std::strstr(exception.what(), "Unexpected acquisition failure") != nullptr);
    }
    acquire_behaviour = behaviour::success;
    diagnostic.clear();
    CHECK(wg_acquire(&options, &out, &error) == WG_OK);
    CHECK(error.code == WG_OK && error.message[0] == '\0');
    CHECK(out != nullptr);
    if (!out)
        return;
    CHECK(wg_get_backend(out) == WG_BACKEND_LOGIND);
    CHECK(wg_get_scope(out) == WG_SCOPE_IDLE_SYSTEM);
    for (auto status : {WG_OK, WG_UNAVAILABLE, WG_LOST}) {
        check_status = status;
        diagnostic = status == WG_OK ? "" : "service disappeared or timed out";
        CHECK(wg_check(out, &error) == status);
        CHECK(error.code == status);
        CHECK(std::strcmp(error.message, diagnostic.c_str()) == 0);
        CHECK(wg_check(out, nullptr) == status);
    }
    for (const auto mode : {behaviour::standard_exception, behaviour::unknown_exception}) {
        check_behaviour = mode;
        CHECK(wg_check(out, &error) == WG_SYSTEM_ERROR);
        CHECK(error.code == WG_SYSTEM_ERROR);
        CHECK(std::strstr(error.message, mode == behaviour::standard_exception
                                             ? "check exception detail"
                                             : "Unexpected check failure") != nullptr);
    }
    check_behaviour = behaviour::success;
    diagnostic = std::string(4096, 'y');
    check_status = WG_LOST;
    CHECK(wg_check(out, &error) == WG_LOST);
    CHECK(std::strlen(error.message) == sizeof(error.message) - 1);
    wg_release(out);
    diagnostic.clear();
    check_status = WG_OK;
    // Both ends of the accepted timeout range reach the platform adapter.
    for (auto timeout : {1U, 60000U}) {
        options.timeout_ms = timeout;
        CHECK(wg_acquire(&options, &out, nullptr) == WG_OK);
        wg_release(out);
    }
}
void ownership() {
    const auto options = wg_default_options();
    const auto before = destroyed;
    {
        wakeguard::guard first(options);
        wakeguard::guard second(options);
        CHECK(first && second);
        second = std::move(first);
        CHECK(destroyed == before + 1);
        // unique_ptr-based owners are empty after moving.
        // NOLINTNEXTLINE(bugprone-use-after-move)
        CHECK(!first && second);
        CHECK(second.backend() == WG_BACKEND_LOGIND);
        CHECK(second.check() == WG_OK);
        auto third = std::move(second);
        // NOLINTNEXTLINE(bugprone-use-after-move)
        CHECK(!second && third);
        third.reset();
        CHECK(!third && destroyed == before + 2);
        third.reset();
        CHECK(destroyed == before + 2);
    }
    CHECK(destroyed == before + 2);
}
} // namespace
wg_guard *wg_platform_acquire(const wg_options &, std::string &detail) {
    ++calls;
    if (acquire_behaviour == behaviour::standard_exception)
        throw std::runtime_error("acquisition exception detail");
    if (acquire_behaviour == behaviour::unknown_exception)
        throw 42;
    if (acquire_behaviour == behaviour::unavailable) {
        detail = diagnostic;
        return nullptr;
    }
    auto guard = std::make_unique<fake_guard>();
    guard->backend = WG_BACKEND_LOGIND;
    return guard.release();
}
int run_tests() {
    invalid_options();
    failures_and_diagnostics();
    ownership();
    return test::result();
}

int main() {
    try {
        return run_tests();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Unexpected test exception: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "Unexpected non-standard test exception\n");
    }
    return 1;
}
