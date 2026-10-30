// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <wakeguard/wakeguard.hpp>

namespace wakeguard {
struct controller::impl {
    using clock = std::chrono::steady_clock;
    std::string application;
    std::string reason;
    std::string window;
    wg_options options;
    std::chrono::milliseconds retry_interval;
    std::chrono::milliseconds check_interval;
    std::mutex mutex;
    std::condition_variable changed;
    bool required = false;
    bool stopping = false;
    bool pending_event = false;
    std::uint64_t generation = 0;
    controller_status status;
    std::thread worker;

    impl(const wg_options &input, std::chrono::milliseconds retry, std::chrono::milliseconds check)
        : application(input.application ? input.application : ""),
          reason(input.reason ? input.reason : ""), window(input.window ? input.window : ""),
          options(input), retry_interval(retry), check_interval(check) {
        if (!input.application || !input.reason || !input.window || application.empty() ||
            reason.empty() || retry.count() <= 0 || check.count() <= 0 ||
            retry > std::chrono::hours(24) || check > std::chrono::hours(24))
            throw std::invalid_argument("Invalid controller options or intervals");
        options.application = application.c_str();
        options.reason = reason.c_str();
        options.window = window.c_str();
        worker = std::thread([this] { run(); });
    }
    void publish(phase state, wg_status error = WG_OK, const char *detail = "",
                 wg_guard *guard = nullptr) {
        status = {state, error, wg_get_backend(guard), wg_get_scope(guard), detail};
        pending_event = true;
    }
    void run() noexcept {
        // Guard must die before this thread exits and before the owner/logger.
        std::unique_ptr<wg_guard, deleter> guard;
        try {
            std::unique_lock<std::mutex> lock(mutex);
            auto next = clock::now();
            while (!stopping) {
                if (!required) {
                    lock.unlock();
                    guard.reset();
                    lock.lock();
                    publish(phase::disabled);
                    next = clock::now();
                    changed.wait(lock, [this] { return stopping || required; });
                    continue;
                }
                if (clock::now() < next) {
                    changed.wait_until(lock, next, [this] { return stopping || !required; });
                    continue;
                }
                if (guard) {
                    lock.unlock();
                    wg_error error{};
                    const auto result = wg_check(guard.get(), &error);
                    lock.lock();
                    if (stopping || !required)
                        continue;
                    if (result == WG_OK || result == WG_UNAVAILABLE) {
                        // A timed-out probe cannot establish loss; retain the lock.
                        if (result != WG_OK || status.error != WG_OK)
                            publish(phase::active, result, error.message, guard.get());
                        next = clock::now() + check_interval;
                        continue;
                    }
                    lock.unlock();
                    guard.reset();
                    lock.lock();
                    publish(phase::unavailable, result, error.message);
                    next = clock::now() + retry_interval;
                    continue;
                }
                const auto attempt_generation = generation;
                publish(phase::acquiring);
                lock.unlock();
                wg_guard *raw = nullptr;
                wg_error error{};
                const auto result = wg_acquire(&options, &raw, &error);
                std::unique_ptr<wg_guard, deleter> acquired(raw);
                lock.lock();
                if (stopping || !required || generation != attempt_generation) {
                    lock.unlock();
                    acquired.reset();
                    lock.lock();
                    next = clock::now();
                    continue;
                }
                if (result == WG_OK) {
                    guard = std::move(acquired);
                    publish(phase::active, WG_OK, "", guard.get());
                    next = clock::now() + check_interval;
                } else {
                    publish(phase::unavailable, result, error.message);
                    next = clock::now() + retry_interval;
                }
            }
        } catch (const std::exception &error) {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            try {
                publish(phase::unavailable, WG_SYSTEM_ERROR, error.what());
            } catch (...) {
                // Allocation failure can prevent even a diagnostic snapshot.
                // stopping is already set; preserve an allocation-free failure event.
                status.state = phase::unavailable;
                status.error = WG_SYSTEM_ERROR;
                status.detail.clear();
                pending_event = true;
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            try {
                publish(phase::unavailable, WG_SYSTEM_ERROR, "Controller worker failed");
            } catch (...) {
                // Allocation failure can prevent even a diagnostic snapshot.
                // stopping is already set; preserve an allocation-free failure event.
                status.state = phase::unavailable;
                status.error = WG_SYSTEM_ERROR;
                status.detail.clear();
                pending_event = true;
            }
        }
        guard.reset();
    }
};
controller::controller(const wg_options &options, std::chrono::milliseconds retry,
                       std::chrono::milliseconds check)
    : value_(std::make_unique<impl>(options, retry, check)) {}
controller::~controller() { shutdown(); }
void controller::set_required(bool required) {
    {
        std::lock_guard<std::mutex> lock(value_->mutex);
        if (value_->stopping || required == value_->required)
            return;
        value_->required = required;
        ++value_->generation;
    }
    value_->changed.notify_one();
}
bool controller::take_event(controller_status &event) {
    std::lock_guard<std::mutex> lock(value_->mutex);
    if (!value_->pending_event)
        return false;
    event = value_->status;
    value_->pending_event = false;
    return true;
}
void controller::shutdown() {
    {
        std::lock_guard<std::mutex> lock(value_->mutex);
        value_->stopping = true;
        value_->required = false;
        ++value_->generation;
    }
    value_->changed.notify_one();
    if (value_->worker.joinable())
        value_->worker.join();
}
} // namespace wakeguard
