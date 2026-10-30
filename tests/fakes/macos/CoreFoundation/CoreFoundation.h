// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#pragma once
// Minimal mock declarations for backend control-flow tests, not Apple ABI tests.
struct test_cf_string;
using CFStringRef = const test_cf_string *;
using CFAllocatorRef = const void *;
using CFStringEncoding = unsigned;
inline constexpr CFAllocatorRef kCFAllocatorDefault = nullptr;
inline constexpr CFStringEncoding kCFStringEncodingUTF8 = 0x08000100;
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, CFStringEncoding);
void CFRelease(CFStringRef);
