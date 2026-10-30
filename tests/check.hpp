// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#pragma once
#include <cstdio>
namespace test {
inline int failures = 0;
inline int assertions = 0;
inline void check(bool condition, const char *expression, int line) {
    ++assertions;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "line %d: %s\n", line, expression);
    }
}
inline int result() {
    std::printf("%d assertions, %d failures\n", assertions, failures);
    return failures ? 1 : 0;
}
} // namespace test
#define CHECK(expression) test::check(bool(expression), #expression, __LINE__)
