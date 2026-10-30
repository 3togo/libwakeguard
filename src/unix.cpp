// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libwakeguard contributors
#include "internal.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
struct object_unref {
    template <class T> void operator()(T *object) const { g_object_unref(object); }
};
struct variant_unref {
    void operator()(GVariant *value) const { g_variant_unref(value); }
};
struct error_free {
    void operator()(GError *error) const { g_error_free(error); }
};
template <class T> using object_ptr = std::unique_ptr<T, object_unref>;
using variant_ptr = std::unique_ptr<GVariant, variant_unref>;
using error_ptr = std::unique_ptr<GError, error_free>;

// Covers bus discovery/authentication as well as method calls. No GLib main
// loop is needed for cancellation. The timer is joined before its state dies.
class deadline {
  public:
    explicit deadline(unsigned int milliseconds)
        : cancel_(g_cancellable_new()), timer_([this, milliseconds] {
              std::unique_lock<std::mutex> lock(mutex_);
              if (!condition_.wait_for(lock, std::chrono::milliseconds(milliseconds),
                                       [this] { return finished_; }))
                  g_cancellable_cancel(cancel_.get());
          }) {}
    ~deadline() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        condition_.notify_one();
        timer_.join();
    }
    GCancellable *get() const { return cancel_.get(); }
    bool expired() const { return g_cancellable_is_cancelled(get()); }

  private:
    object_ptr<GCancellable> cancel_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool finished_ = false;
    std::thread timer_;
};

std::string take_error(GError *raw) {
    error_ptr error(raw);
    return error ? error->message : "Unexpected empty D-Bus response";
}

struct unix_guard final : wg_guard {
    object_ptr<GDBusConnection> connection;
    int fd = -1;
    std::string service;
    std::string owner;
    ~unix_guard() override {
        if (fd >= 0)
            close(fd);
        // A private connection per guard makes disconnect an independent
        // release mechanism, including a timed-out/ambiguous portal reply.
        // Closing is asynchronous, so destructors do not wait for D-Bus.
        if (connection && !g_dbus_connection_is_closed(connection.get()))
            g_dbus_connection_close(connection.get(), nullptr, nullptr, nullptr);
    }
    wg_status check(std::string &detail) const override;
};

variant_ptr call(GDBusConnection *connection, const char *destination, const char *path,
                 const char *interface, const char *method, GVariant *parameters,
                 const GVariantType *reply_type, deadline &limit, std::string &detail) {
    GError *error = nullptr;
    variant_ptr reply(g_dbus_connection_call_sync(connection, destination, path, interface, method,
                                                  parameters, reply_type, G_DBUS_CALL_FLAGS_NONE,
                                                  60000, limit.get(), &error));
    if (!reply)
        detail = take_error(error);
    return reply;
}

std::string get_owner(GDBusConnection *connection, const std::string &service, deadline &limit,
                      std::string &detail) {
    auto reply = call(connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                      "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", service.c_str()),
                      G_VARIANT_TYPE("(s)"), limit, detail);
    if (!reply)
        return {};
    const char *owner = nullptr;
    g_variant_get(reply.get(), "(&s)", &owner);
    return owner;
}

wg_status unix_guard::check(std::string &detail) const {
    if (g_dbus_connection_is_closed(connection.get())) {
        detail = "Inhibitor bus connection was closed";
        return WG_LOST;
    }
    deadline limit(1000);
    const auto current_owner = get_owner(connection.get(), service, limit, detail);
    if (current_owner.empty()) {
        // A timeout cannot establish loss; let the caller retry the probe.
        if (limit.expired())
            return WG_UNAVAILABLE;
        detail = "Cannot verify inhibitor service: " + detail;
        return WG_LOST;
    }
    if (current_owner != owner) {
        detail = "Inhibitor service restarted; release and reacquire the guard";
        return WG_LOST;
    }
    return WG_OK;
}

std::unique_ptr<unix_guard> connect(wg_backend backend, deadline &limit, std::string &detail) {
    GError *error = nullptr;
    const auto bus = backend == WG_BACKEND_PORTAL ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
    std::unique_ptr<gchar, decltype(&g_free)> address(
        g_dbus_address_get_for_bus_sync(bus, limit.get(), &error), g_free);
    if (!address) {
        detail = take_error(error);
        return {};
    }
    auto guard = std::make_unique<unix_guard>();
    guard->backend = backend;
    guard->connection.reset(g_dbus_connection_new_for_address_sync(
        // GDBusConnectionFlags is a bitmask; combinations are valid.
        address.get(),
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        GDBusConnectionFlags(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                             G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
        nullptr, limit.get(), &error));
    if (!guard->connection) {
        detail = take_error(error);
        return {};
    }
    g_dbus_connection_set_exit_on_close(guard->connection.get(), FALSE);
    switch (backend) {
    case WG_BACKEND_PORTAL:
        guard->service = "org.freedesktop.portal.Desktop";
        break;
    case WG_BACKEND_LOGIND:
        guard->service = "org.freedesktop.login1";
        break;
    case WG_BACKEND_CONSOLEKIT:
        guard->service = "org.freedesktop.ConsoleKit";
        break;
    default:
        detail = "Backend is not a Unix bus backend";
        return {};
    }
    guard->owner = get_owner(guard->connection.get(), guard->service, limit, detail);
    if (guard->owner.empty() && !limit.expired()) {
        // Permit service activation, including portals installed but not yet
        // running. A missing activation file is just an unavailable backend.
        auto started = call(guard->connection.get(), "org.freedesktop.DBus",
                            "/org/freedesktop/DBus", "org.freedesktop.DBus", "StartServiceByName",
                            g_variant_new("(su)", guard->service.c_str(), 0U),
                            G_VARIANT_TYPE("(u)"), limit, detail);
        if (started)
            guard->owner = get_owner(guard->connection.get(), guard->service, limit, detail);
    }
    if (guard->owner.empty())
        return {};
    return guard;
}

bool acquire_fd(unix_guard &guard, const wg_options &options, deadline &limit,
                std::string &detail) {
    const bool logind = guard.backend == WG_BACKEND_LOGIND;
    GUnixFDList *raw_fds = nullptr;
    GError *error = nullptr;
    variant_ptr reply(g_dbus_connection_call_with_unix_fd_list_sync(
        guard.connection.get(), guard.owner.c_str(),
        logind ? "/org/freedesktop/login1" : "/org/freedesktop/ConsoleKit/Manager",
        logind ? "org.freedesktop.login1.Manager" : "org.freedesktop.ConsoleKit.Manager", "Inhibit",
        g_variant_new("(ssss)", options.allow_suspend_block ? "sleep:idle" : "idle",
                      options.application, options.reason, "block"),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 60000, nullptr, &raw_fds, limit.get(),
        &error));
    object_ptr<GUnixFDList> fds(raw_fds);
    if (!reply) {
        detail = take_error(error);
        return false;
    }
    gint32 index = -1;
    g_variant_get(reply.get(), "(h)", &index);
    if (!fds || index < 0 || index >= g_unix_fd_list_get_length(fds.get())) {
        detail = "Inhibit returned an invalid file-descriptor index";
        return false;
    }
    guard.fd = g_unix_fd_list_get(fds.get(), index, &error);
    if (guard.fd < 0) {
        detail = take_error(error);
        return false;
    }
    guard.scope = options.allow_suspend_block ? WG_SCOPE_SUSPEND : WG_SCOPE_IDLE_SYSTEM;
    return true;
}

struct context_scope {
    GMainContext *context;
    explicit context_scope(GMainContext *value) : context(value) {
        g_main_context_push_thread_default(context);
    }
    ~context_scope() { g_main_context_pop_thread_default(context); }
};
struct subscription_scope {
    GDBusConnection *connection;
    guint id;
    ~subscription_scope() { g_dbus_connection_signal_unsubscribe(connection, id); }
};
struct portal_response {
    std::string handle;
    bool received = false;
    guint code = 2;
};
void portal_signal(GDBusConnection *, const gchar *, const gchar *path, const gchar *,
                   const gchar *, GVariant *parameters, gpointer data) {
    auto &response = *static_cast<portal_response *>(data);
    if (response.handle != path || !g_variant_is_of_type(parameters, G_VARIANT_TYPE("(ua{sv})")))
        return;
    variant_ptr code(g_variant_get_child_value(parameters, 0));
    response.code = g_variant_get_uint32(code.get());
    response.received = true;
}

bool acquire_portal(unix_guard &guard, const wg_options &options, deadline &limit,
                    std::string &detail) {
    // Subscribe before the call, then dispatch on our own context: a fast
    // Response signal may arrive before the method reply. Never treat the
    // returned request handle alone as successful inhibition.
    std::unique_ptr<GMainContext, decltype(&g_main_context_unref)> context(g_main_context_new(),
                                                                           g_main_context_unref);
    portal_response response;
    context_scope pushed(context.get());
    const guint subscription = g_dbus_connection_signal_subscribe(
        guard.connection.get(), guard.owner.c_str(), "org.freedesktop.portal.Request", "Response",
        nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE, portal_signal, &response, nullptr);
    subscription_scope subscribed{guard.connection.get(), subscription};
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "reason", g_variant_new_string(options.reason));
    auto reply =
        call(guard.connection.get(), guard.owner.c_str(), "/org/freedesktop/portal/desktop",
             "org.freedesktop.portal.Inhibit", "Inhibit",
             g_variant_new("(su@a{sv})", options.window, 4U, g_variant_builder_end(&builder)),
             G_VARIANT_TYPE("(o)"), limit, detail);
    if (reply) {
        const char *handle = nullptr;
        g_variant_get(reply.get(), "(&o)", &handle);
        response.handle = handle;
        while (!response.received && !limit.expired()) {
            while (!response.received && !limit.expired() &&
                   g_main_context_iteration(context.get(), FALSE)) {
            }
            if (!response.received)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!response.received)
            detail = "Portal did not confirm inhibition before the deadline";
        else if (response.code != 0)
            detail = "Portal refused inhibition (response " + std::to_string(response.code) + ")";
    }
    if (!reply || !response.received || response.code != 0)
        return false;
    guard.scope = WG_SCOPE_SUSPEND;
    return true;
}
} // namespace

wg_guard *wg_platform_acquire(const wg_options &options, std::string &detail) {
    if (!g_utf8_validate(options.application, -1, nullptr) ||
        !g_utf8_validate(options.reason, -1, nullptr) ||
        !g_utf8_validate(options.window, -1, nullptr)) {
        detail = "Options must contain valid UTF-8";
        return nullptr;
    }
    if (options.backend == WG_BACKEND_NATIVE) {
        detail = "Native backend is unavailable on Unix; use a bus backend";
        return nullptr;
    }
    const auto end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeout_ms);
    const wg_backend candidates[] = {WG_BACKEND_PORTAL, WG_BACKEND_LOGIND, WG_BACKEND_CONSOLEKIT};
    const auto eligible = [&options](wg_backend backend) {
        return (options.backend == WG_BACKEND_AUTO || options.backend == backend) &&
               (backend != WG_BACKEND_PORTAL || options.allow_suspend_block);
    };
    auto remaining_candidates =
        std::count_if(std::begin(candidates), std::end(candidates), eligible);
    for (const auto backend : candidates) {
        if (options.backend != WG_BACKEND_AUTO && options.backend != backend)
            continue;
        if (backend == WG_BACKEND_PORTAL && !options.allow_suspend_block)
            continue;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   end - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) {
            detail += " (total acquisition deadline exceeded)";
            break;
        }
        // Reserve time for later candidates: a silent portal or stalled logind
        // must not consume every retry without ever trying a usable fallback.
        const auto budget =
            static_cast<unsigned int>(std::max<std::int64_t>(1, remaining / remaining_candidates));
        --remaining_candidates;
        deadline limit(budget);
        std::string attempt;
        auto guard = connect(backend, limit, attempt);
        if (guard && (backend == WG_BACKEND_PORTAL ? acquire_portal(*guard, options, limit, attempt)
                                                   : acquire_fd(*guard, options, limit, attempt)))
            return guard.release();
        if (!detail.empty())
            detail += "; ";
        detail += std::string(wg_backend_name(backend)) + ": " + attempt;
        if (limit.expired()) {
            detail += remaining_candidates ? " (backend acquisition deadline exceeded)"
                                           : " (total acquisition deadline exceeded)";
        }
    }
    return nullptr;
}
