// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#pragma once
#include <CoreFoundation/CoreFoundation.h>
using IOPMAssertionID = unsigned;
using IOReturn = int;
inline constexpr IOPMAssertionID kIOPMNullAssertionID = 0;
inline constexpr IOReturn kIOReturnSuccess = 0;
inline constexpr unsigned kIOPMAssertionLevelOn = 255;
extern CFStringRef kIOPMAssertionTypePreventUserIdleSystemSleep;
IOReturn IOPMAssertionCreateWithName(CFStringRef, unsigned, CFStringRef, IOPMAssertionID *);
IOReturn IOPMAssertionRelease(IOPMAssertionID);
