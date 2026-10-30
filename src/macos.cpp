// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "internal.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <memory>

namespace {
struct native_guard final : wg_guard {
    IOPMAssertionID assertion = kIOPMNullAssertionID;
    ~native_guard() override {
        if (assertion != kIOPMNullAssertionID)
            IOPMAssertionRelease(assertion);
    }
    wg_status check(std::string &) const override { return WG_OK; }
};
} // namespace
wg_guard *wg_platform_acquire(const wg_options &options, std::string &detail) {
    if (options.backend != WG_BACKEND_AUTO && options.backend != WG_BACKEND_NATIVE) {
        detail = "Requested backend is unavailable on macOS";
        return nullptr;
    }
    auto guard = std::make_unique<native_guard>();
    guard->backend = WG_BACKEND_NATIVE;
    CFStringRef reason =
        CFStringCreateWithCString(kCFAllocatorDefault, options.reason, kCFStringEncodingUTF8);
    if (!reason) {
        detail = "Reason is not valid UTF-8";
        return nullptr;
    }
    IOPMAssertionID assertion = kIOPMNullAssertionID;
    const auto result = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                                    kIOPMAssertionLevelOn, reason, &assertion);
    CFRelease(reason);
    if (result != kIOReturnSuccess) {
        detail = "IOPMAssertionCreateWithName failed (IOKit error " + std::to_string(result) + ")";
        return nullptr;
    }
    // A failed call does not transfer ownership of its output parameter.
    guard->assertion = assertion;
    return guard.release();
}
