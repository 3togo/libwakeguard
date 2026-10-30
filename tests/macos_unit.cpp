// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "check.hpp"
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <wakeguard/wakeguard.hpp>
struct test_cf_string {
    std::string text;
};
namespace {
const test_cf_string assertion_type{"PreventUserIdleSystemSleep"};
bool string_failure = false;
bool assertion_failure = false;
int strings = 0;
int creates = 0;
int releases = 0;
unsigned next_id = 0;
std::set<IOPMAssertionID> active;
std::string recorded_reason;
} // namespace
CFStringRef kIOPMAssertionTypePreventUserIdleSystemSleep = &assertion_type;
CFStringRef CFStringCreateWithCString(CFAllocatorRef allocator, const char *text,
                                      CFStringEncoding encoding) {
    CHECK(allocator == kCFAllocatorDefault && encoding == kCFStringEncodingUTF8);
    if (string_failure)
        return nullptr;
    ++strings;
    return new test_cf_string{text};
}
void CFRelease(CFStringRef string) {
    CHECK(string != nullptr && string != &assertion_type);
    --strings;
    delete string;
}
IOReturn IOPMAssertionCreateWithName(CFStringRef type, unsigned level, CFStringRef reason,
                                     IOPMAssertionID *id) {
    CHECK(type == kIOPMAssertionTypePreventUserIdleSystemSleep);
    CHECK(level == kIOPMAssertionLevelOn);
    CHECK(strings == 1);
    recorded_reason = reason->text;
    ++creates;
    if (assertion_failure) {
        // A failing call does not transfer ownership of an output identifier.
        *id = 9999;
        return -42;
    }
    *id = ++next_id;
    active.insert(*id);
    return kIOReturnSuccess;
}
IOReturn IOPMAssertionRelease(IOPMAssertionID id) {
    ++releases;
    CHECK(active.erase(id) == 1);
    return kIOReturnSuccess;
}
int run_tests() {
    auto options = wg_default_options();
    wg_guard *out = nullptr;
    wg_error error{};
    string_failure = true;
    CHECK(wg_acquire(&options, &out, &error) == WG_UNAVAILABLE);
    CHECK(out == nullptr && strings == 0 && creates == 0 && releases == 0);
    CHECK(std::strstr(error.message, "valid UTF-8") != nullptr);
    string_failure = false;
    assertion_failure = true;
    CHECK(wg_acquire(&options, &out, &error) == WG_UNAVAILABLE);
    CHECK(out == nullptr && strings == 0 && active.empty());
    CHECK(std::strstr(error.message, "IOKit error -42") != nullptr);
    CHECK(releases == 0);
    assertion_failure = false;
    options.reason = "Transferring files";
    {
        wakeguard::guard first(options);
        options.backend = WG_BACKEND_NATIVE;
        wakeguard::guard second(options);
        CHECK(first.backend() == WG_BACKEND_NATIVE && first.scope() == WG_SCOPE_IDLE_SYSTEM);
        CHECK(first.check() == WG_OK);
        CHECK(active.size() == 2 && strings == 0 && recorded_reason == options.reason);
        std::thread worker([owner = std::move(first)]() mutable { owner.reset(); });
        worker.join();
        CHECK(active.size() == 1);
        second.reset();
        CHECK(active.empty() && releases == 2);
    }
    CHECK(active.empty() && strings == 0 && releases == 2);
    options.backend = WG_BACKEND_LOGIND;
    const auto previous_creates = creates;
    CHECK(wg_acquire(&options, &out, &error) == WG_UNAVAILABLE);
    CHECK(std::strstr(error.message, "unavailable on macOS") != nullptr);
    CHECK(creates == previous_creates && strings == 0);
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
