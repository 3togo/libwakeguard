// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#pragma once
#include <chrono>
#include <cstdint>
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
enum class phase : std::uint8_t { disabled, acquiring, active, unavailable };
struct controller_status {
    phase state = phase::disabled;
    wg_status error = WG_OK;
    wg_backend backend = WG_BACKEND_AUTO;
    wg_scope scope = WG_SCOPE_IDLE_SYSTEM;
    std::string detail;
};

// A single worker owns the inhibitor, runs acquisition/health checks and retries
// failures with backoff. Repeated requests are idempotent and do no bus/OS work.
// Options strings are snapshotted; retry/check intervals must be 1ms..24h.
// take_event() returns the latest transition,
// including backend failure details, for logging on the application's thread.
class WG_EXPORT controller {
  public:
    explicit controller(const wg_options &options,
                        std::chrono::milliseconds retry = std::chrono::seconds(30),
                        std::chrono::milliseconds check = std::chrono::seconds(30));
    ~controller();
    controller(const controller &) = delete;
    controller &operator=(const controller &) = delete;
    void set_required(bool required);
    bool take_event(controller_status &event);
    // Joins the worker and releases before returning. Late acquisition results
    // are discarded after disable/shutdown. May wait for the acquisition budget
    // or a native OS call that has no cancellation API. Idempotent; no restart.
    void shutdown();

  private:
    struct impl;
    std::unique_ptr<impl> value_;
};
} // namespace wakeguard
