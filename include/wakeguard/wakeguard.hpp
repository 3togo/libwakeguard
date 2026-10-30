// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <wakeguard/wakeguard.h>

namespace wakeguard {
class acquisition_error : public std::runtime_error {
  public:
    explicit acquisition_error(const wg_error &error)
        : std::runtime_error(error.message), code_(error.code) {}
    wg_status code() const noexcept { return code_; }

  private:
    wg_status code_;
};
struct deleter {
    void operator()(wg_guard *guard) const noexcept { wg_release(guard); }
};
// Move-only RAII owner. Acquisition/check are blocking; dispatch onto a worker
// in event-driven applications. Release does not run a synchronous D-Bus call.
class guard {
  public:
    guard() = default;
    explicit guard(const wg_options &options) {
        wg_guard *raw = nullptr;
        wg_error error{};
        if (wg_acquire(&options, &raw, &error) != WG_OK)
            throw acquisition_error(error);
        value_.reset(raw);
    }
    wg_backend backend() const noexcept { return wg_get_backend(value_.get()); }
    wg_scope scope() const noexcept { return wg_get_scope(value_.get()); }
    explicit operator bool() const noexcept { return bool(value_); }
    wg_status check(wg_error *error = nullptr) const { return wg_check(value_.get(), error); }
    void reset() noexcept { value_.reset(); }

  private:
    std::unique_ptr<wg_guard, deleter> value_;
};
} // namespace wakeguard
