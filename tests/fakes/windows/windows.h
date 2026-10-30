// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#pragma once
// Minimal test interface, not a Windows ABI/SDK replacement. Only the Linux
// backend-control-flow unit target sees this directory.
#include <cstdint>
struct test_power_request {
    unsigned id;
};
using DWORD = std::uint32_t;
using BOOL = int;
using HANDLE = test_power_request *;
inline test_power_request invalid_test_handle{};
inline const HANDLE INVALID_HANDLE_VALUE = &invalid_test_handle;
constexpr unsigned CP_UTF8 = 65001;
constexpr DWORD MB_ERR_INVALID_CHARS = 8;
constexpr unsigned POWER_REQUEST_CONTEXT_VERSION = 0;
constexpr DWORD POWER_REQUEST_CONTEXT_SIMPLE_STRING = 1;
enum POWER_REQUEST_TYPE : std::uint8_t { PowerRequestDisplayRequired, PowerRequestSystemRequired };
struct REASON_CONTEXT {
    unsigned Version;
    DWORD Flags;
    struct {
        wchar_t *SimpleReasonString;
    } Reason;
};
int MultiByteToWideChar(unsigned, DWORD, const char *, int, wchar_t *, int);
DWORD GetLastError();
HANDLE PowerCreateRequest(REASON_CONTEXT *);
BOOL PowerSetRequest(HANDLE, POWER_REQUEST_TYPE);
BOOL PowerClearRequest(HANDLE, POWER_REQUEST_TYPE);
BOOL CloseHandle(HANDLE);
