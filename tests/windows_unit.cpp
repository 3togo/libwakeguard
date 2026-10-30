// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "check.hpp"
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <wakeguard/wakeguard.hpp>
#include <windows.h>
namespace {
int conversion_failure = 0;
int conversion_calls = 0;
bool create_failure = false;
bool set_failure = false;
bool clear_failure = false;
unsigned next_id = 0;
std::set<HANDLE> active;
std::vector<std::string> events;
std::wstring recorded_reason;
void reset() {
    CHECK(active.empty());
    conversion_failure = 0;
    conversion_calls = 0;
    create_failure = set_failure = clear_failure = false;
    events.clear();
}
void expect_failure(const char *message) {
    auto options = wg_default_options();
    wg_error error{};
    wg_guard *guard = nullptr;
    CHECK(wg_acquire(&options, &guard, &error) == WG_UNAVAILABLE);
    CHECK(!guard);
    CHECK(std::strstr(error.message, message) != nullptr);
    CHECK(std::strstr(error.message, "123") != nullptr);
    CHECK(active.empty());
}
} // namespace
int MultiByteToWideChar(unsigned codepage, DWORD flags, const char *input, int length,
                        wchar_t *output, int capacity) {
    CHECK(codepage == CP_UTF8 && flags == MB_ERR_INVALID_CHARS && length == -1);
    ++conversion_calls;
    if (conversion_calls == conversion_failure)
        return 0;
    const int size = static_cast<int>(std::strlen(input)) + 1;
    if (!output)
        return size;
    CHECK(capacity == size);
    // ASCII fixture conversion only. Windows itself implements/validates UTF-8.
    for (int i = 0; i < size; ++i)
        output[i] = static_cast<unsigned char>(input[i]);
    return size;
}
DWORD GetLastError() { return 123; }
HANDLE PowerCreateRequest(REASON_CONTEXT *context) {
    CHECK(context->Version == POWER_REQUEST_CONTEXT_VERSION);
    CHECK(context->Flags == POWER_REQUEST_CONTEXT_SIMPLE_STRING);
    recorded_reason = context->Reason.SimpleReasonString;
    events.emplace_back("create");
    if (create_failure)
        return INVALID_HANDLE_VALUE;
    auto *handle = new test_power_request{++next_id};
    active.insert(handle);
    return handle;
}
BOOL PowerSetRequest(HANDLE handle, POWER_REQUEST_TYPE type) {
    CHECK(active.count(handle) == 1 && type == PowerRequestSystemRequired);
    events.emplace_back("set");
    return !set_failure;
}
BOOL PowerClearRequest(HANDLE handle, POWER_REQUEST_TYPE type) {
    CHECK(active.count(handle) == 1 && type == PowerRequestSystemRequired);
    events.emplace_back("clear");
    return !clear_failure;
}
BOOL CloseHandle(HANDLE handle) {
    events.emplace_back("close");
    const auto erased = active.erase(handle);
    CHECK(erased == 1);
    if (erased)
        delete handle;
    return 1;
}
int run_tests() {
    for (const int stage : {1, 2}) {
        reset();
        conversion_failure = stage;
        expect_failure("UTF-8 reason conversion failed");
        CHECK(events.empty());
    }
    reset();
    create_failure = true;
    expect_failure("PowerCreateRequest failed");
    CHECK(events == std::vector<std::string>{"create"});
    reset();
    set_failure = true;
    expect_failure("PowerSetRequest failed");
    CHECK((events == std::vector<std::string>{"create", "set", "close"}));
    reset();
    auto options = wg_default_options();
    options.reason = "Downloading files";
    {
        wakeguard::guard first(options);
        options.backend = WG_BACKEND_NATIVE;
        wakeguard::guard second(options);
        CHECK(active.size() == 2);
        CHECK(recorded_reason == L"Downloading files");
        CHECK(first.backend() == WG_BACKEND_NATIVE && first.scope() == WG_SCOPE_IDLE_SYSTEM);
        CHECK(first.check() == WG_OK);
        std::thread worker([owner = std::move(first)]() mutable { owner.reset(); });
        worker.join();
        CHECK(active.size() == 1);
        second.reset();
        CHECK(active.empty());
    }
    CHECK((events == std::vector<std::string>{"create", "set", "create", "set", "clear", "close",
                                              "clear", "close"}));
    reset();
    clear_failure = true;
    { wakeguard::guard guard(options); }
    CHECK(active.empty()); // Even failed PowerClearRequest must be followed by CloseHandle.
    reset();
    options.backend = WG_BACKEND_LOGIND;
    wg_guard *out = nullptr;
    wg_error error{};
    CHECK(wg_acquire(&options, &out, &error) == WG_UNAVAILABLE);
    CHECK(std::strstr(error.message, "unavailable on Windows") != nullptr);
    CHECK(events.empty() && conversion_calls == 0);
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
