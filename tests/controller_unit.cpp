// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "check.hpp"
#include "internal.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <wakeguard/wakeguard.hpp>

namespace {
using namespace std::chrono_literals;
struct fixture {
    std::mutex mutex;
    std::condition_variable changed;
    bool block = false;
    bool failure = false;
    wg_status health = WG_OK;
    int attempts = 0;
    int releases = 0;
    int active = 0;
    int checks = 0;
    std::string application;
    std::string reason;
    bool wait(const std::function<bool()> &condition) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, condition);
    }
    void reset() {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(active == 0);
        block = failure = false;
        health = WG_OK;
        attempts = releases = active = checks = 0;
    }
} service;
struct fake_guard final : wg_guard {
    ~fake_guard() override {
        std::lock_guard<std::mutex> lock(service.mutex);
        --service.active;
        ++service.releases;
        service.changed.notify_all();
    }
    wg_status check(std::string &detail) const override {
        std::lock_guard<std::mutex> lock(service.mutex);
        ++service.checks;
        detail = service.health == WG_OK ? "" : "health detail from production adapter";
        service.changed.notify_all();
        return service.health;
    }
};
bool event(wakeguard::controller &controller, wakeguard::phase phase,
           wakeguard::controller_status &status) {
    const auto end = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < end) {
        if (controller.take_event(status) && status.state == phase)
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}
void idempotence_and_snapshots() {
    service.reset();
    std::string name = "aMule";
    std::string reason = "Transferring files";
    auto options = wg_default_options();
    options.application = name.c_str();
    options.reason = reason.c_str();
    wakeguard::controller controller(options, 100ms, 10s);
    name = "overwritten";
    reason.clear();
    for (int i = 0; i < 1000; ++i)
        controller.set_required(true);
    CHECK(service.wait([] { return service.active == 1; }));
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        CHECK(service.attempts == 1);
        CHECK(service.application == "aMule" && service.reason == "Transferring files");
    }
    wakeguard::controller_status status;
    CHECK(event(controller, wakeguard::phase::active, status));
    CHECK(status.backend == WG_BACKEND_LOGIND && status.scope == WG_SCOPE_IDLE_SYSTEM);
    for (int i = 0; i < 1000; ++i)
        controller.set_required(false);
    CHECK(service.wait([] { return service.releases == 1; }));
    CHECK(event(controller, wakeguard::phase::disabled, status));
    controller.shutdown();
    controller.shutdown();
    controller.set_required(true);
    std::lock_guard<std::mutex> lock(service.mutex);
    CHECK(service.attempts == 1 && service.active == 0);
}
void disable_during_acquisition() {
    service.reset();
    service.block = true;
    wakeguard::controller controller(wg_default_options(), 100ms, 10s);
    controller.set_required(true);
    CHECK(service.wait([] { return service.attempts == 1; }));
    // This must not wait for the backend: unblock only after the caller returns.
    controller.set_required(false);
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.block = false;
        service.changed.notify_all();
    }
    CHECK(service.wait([] { return service.releases == 1; }));
    wakeguard::controller_status status;
    CHECK(event(controller, wakeguard::phase::disabled, status));
    controller.set_required(true);
    CHECK(service.wait([] { return service.attempts == 2 && service.active == 1; }));
    controller.shutdown();
    CHECK(service.wait([] { return service.active == 0 && service.releases == 2; }));
}
void stale_generation_is_discarded() {
    service.reset();
    service.block = true;
    wakeguard::controller controller(wg_default_options(), 100ms, 10s);
    controller.set_required(true);
    CHECK(service.wait([] { return service.attempts == 1; }));
    controller.set_required(false);
    controller.set_required(true);
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.block = false;
        service.changed.notify_all();
    }
    CHECK(service.wait(
        [] { return service.attempts == 2 && service.releases == 1 && service.active == 1; }));
    controller.shutdown();
}
void errors_retry_and_recovery() {
    service.reset();
    service.failure = true;
    wakeguard::controller controller(wg_default_options(), 200ms, 20ms);
    controller.set_required(true);
    wakeguard::controller_status status;
    CHECK(event(controller, wakeguard::phase::unavailable, status));
    CHECK(status.error == WG_UNAVAILABLE &&
          status.detail == "policy refusal from production adapter");
    // Repeated ticks must not turn backoff into repeated synchronous requests.
    for (int i = 0; i < 1000; ++i)
        controller.set_required(true);
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        CHECK(service.attempts == 1);
        service.failure = false;
    }
    CHECK(service.wait([] { return service.attempts == 2 && service.active == 1; }));
    CHECK(event(controller, wakeguard::phase::active, status));
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.health = WG_UNAVAILABLE;
    }
    CHECK(event(controller, wakeguard::phase::active, status));
    CHECK(status.error == WG_UNAVAILABLE &&
          status.detail == "health detail from production adapter");
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        CHECK(service.releases == 0); // Uncertain health must not discard an owned inhibitor.
        service.health = WG_OK;
    }
    CHECK(event(controller, wakeguard::phase::active, status));
    CHECK(status.error == WG_OK && status.detail.empty());
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.health = WG_LOST;
    }
    CHECK(event(controller, wakeguard::phase::unavailable, status));
    CHECK(status.error == WG_LOST && status.detail == "health detail from production adapter");
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.health = WG_OK;
    }
    CHECK(service.wait([] { return service.attempts == 3 && service.active == 1; }));
    controller.shutdown();
    CHECK(service.wait([] { return service.active == 0 && service.releases == 2; }));
}
void shutdown_during_acquisition() {
    service.reset();
    service.block = true;
    wakeguard::controller controller(wg_default_options(), 100ms, 10s);
    controller.set_required(true);
    CHECK(service.wait([] { return service.attempts == 1; }));
    std::thread stopper([&controller] { controller.shutdown(); });
    // A false desired state is observable through set_required even while shutdown joins.
    controller.set_required(false);
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.block = false;
        service.changed.notify_all();
    }
    stopper.join();
    CHECK(service.wait([] { return service.active == 0 && service.releases == 1; }));
}
} // namespace
wg_guard *wg_platform_acquire(const wg_options &options, std::string &detail) {
    std::unique_lock<std::mutex> lock(service.mutex);
    ++service.attempts;
    service.application = options.application;
    service.reason = options.reason;
    service.changed.notify_all();
    service.changed.wait(lock, [] { return !service.block; });
    if (service.failure) {
        detail = "policy refusal from production adapter";
        return nullptr;
    }
    auto guard = std::make_unique<fake_guard>();
    guard->backend = WG_BACKEND_LOGIND;
    ++service.active;
    service.changed.notify_all();
    return guard.release();
}
int main() {
    try {
        idempotence_and_snapshots();
        disable_during_acquisition();
        stale_generation_is_discarded();
        errors_retry_and_recovery();
        shutdown_during_acquisition();
        return test::result();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Unexpected controller test exception: %s\n", error.what());
        return 1;
    }
}
