// SPDX-License-Identifier: Apache-2.0
// The C API (include/wsrv/wsrv.h) as a thin translation of EmbeddedServer: a pointer check, one
// call, a status code. No behaviour lives here (ADR-0016 section 9): a bug in connection handling
// is fixed in the C++ classes below, once, for every language.
#include "wsrv/wsrv.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "capi/c_status.hpp"
#include "capi/embedded_config.hpp"
#include "capi/embedded_server.hpp"

struct wsrv_config {
    wsrv::EmbeddedConfig value;
};

struct wsrv_server {
    std::unique_ptr<wsrv::EmbeddedServer> impl;
};

namespace {

// What an event with no payload points at: the documented "data is never NULL".
constexpr std::array<std::uint8_t, 1> kNoBytes = {0};

/// The event payload as the C API's unsigned bytes. The one audited reinterpret_cast in the C
/// layer: std::byte and unsigned char alias any object and have equal size.
const std::uint8_t* c_bytes(wsrv::ByteView bytes) noexcept {
    if (bytes.empty()) {
        return kNoBytes.data();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): audited, see comment above.
    return reinterpret_cast<const std::uint8_t*>(bytes.data());
}

void fill(wsrv_error* error, wsrv_status status, std::string_view message) noexcept {
    if (error == nullptr) {
        return;
    }
    error->status = status;
    const std::size_t room = sizeof(error->message) - 1;
    const std::size_t size = std::min(message.size(), room);
    std::copy_n(message.data(), size, error->message);
    error->message[size] = '\0';
}

wsrv_status report(const wsrv::Error& error, wsrv_error* out) noexcept {
    const wsrv_status status = wsrv::to_c_status(error.code);
    fill(out, status, error.context);
    return status;
}

wsrv_status ok(wsrv_error* error) noexcept {
    fill(error, WSRV_OK, {});
    return WSRV_OK;
}

wsrv_status invalid(wsrv_error* error, std::string_view why) noexcept {
    fill(error, WSRV_ERR_INVALID_ARGUMENT, why);
    return WSRV_ERR_INVALID_ARGUMENT;
}

/// Run `body`; nothing may escape the C boundary. Only the exceptions the code base can throw are
/// caught (out of memory, and the standard library's own), each mapped to a status.
template <class Body>
wsrv_status guarded(wsrv_error* error, Body&& body) noexcept {
    try {
        return std::forward<Body>(body)();
    } catch (const std::bad_alloc&) {
        fill(error, WSRV_ERR_RESOURCE, "out of memory");
        return WSRV_ERR_RESOURCE;
    } catch (const std::exception&) {
        fill(error, WSRV_ERR_INTERNAL, "unexpected exception");
        return WSRV_ERR_INTERNAL;
    }
}

template <class T>
wsrv_status from_result(const wsrv::Result<T>& result, wsrv_error* error) noexcept {
    return result ? ok(error) : report(result.error(), error);
}

wsrv_event_type to_c_type(wsrv::HostEventType type) noexcept {
    switch (type) {
    case wsrv::HostEventType::Open:
        return WSRV_EV_OPEN;
    case wsrv::HostEventType::Text:
        return WSRV_EV_TEXT;
    case wsrv::HostEventType::Binary:
        return WSRV_EV_BINARY;
    case wsrv::HostEventType::Close:
        return WSRV_EV_CLOSE;
    case wsrv::HostEventType::Error:
        return WSRV_EV_ERROR;
    }
    return WSRV_EV_ERROR;
}

}  // namespace

extern "C" {

uint32_t wsrv_abi_version(void) {
    return WSRV_ABI_VERSION;
}

const char* wsrv_status_name(int status) {
    switch (status) {
    case WSRV_OK:
        return "WSRV_OK";
    case WSRV_TIMEOUT:
        return "WSRV_TIMEOUT";
    case WSRV_ERR_INVALID_ARGUMENT:
        return "WSRV_ERR_INVALID_ARGUMENT";
    case WSRV_ERR_INVALID_CONFIG:
        return "WSRV_ERR_INVALID_CONFIG";
    case WSRV_ERR_ADDRESS_IN_USE:
        return "WSRV_ERR_ADDRESS_IN_USE";
    case WSRV_ERR_NO_SUCH_CONN:
        return "WSRV_ERR_NO_SUCH_CONN";
    case WSRV_ERR_BACKPRESSURE:
        return "WSRV_ERR_BACKPRESSURE";
    case WSRV_ERR_INVALID_UTF8:
        return "WSRV_ERR_INVALID_UTF8";
    case WSRV_ERR_WRONG_PROCESS:
        return "WSRV_ERR_WRONG_PROCESS";
    case WSRV_ERR_RESOURCE:
        return "WSRV_ERR_RESOURCE";
    case WSRV_ERR_IO:
        return "WSRV_ERR_IO";
    case WSRV_ERR_INTERNAL:
        return "WSRV_ERR_INTERNAL";
    }
    return "WSRV_ERR_UNKNOWN";
}

// ---- configuration -----------------------------------------------------------------------------

wsrv_config* wsrv_config_new(void) {
    try {
        return std::make_unique<wsrv_config>().release();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void wsrv_config_free(wsrv_config* config) {
    const std::unique_ptr<wsrv_config> owner{config};
}

wsrv_status
wsrv_config_set(wsrv_config* config, const char* key, const char* value, wsrv_error* error) {
    if (config == nullptr || key == nullptr || value == nullptr) {
        return invalid(error, "wsrv_config_set: NULL argument");
    }
    return guarded(error, [&] { return from_result(config->value.set(key, value), error); });
}

// ---- lifecycle ---------------------------------------------------------------------------------

wsrv_status wsrv_server_create(const wsrv_config* config, wsrv_server** out, wsrv_error* error) {
    if (out == nullptr) {
        return invalid(error, "wsrv_server_create: out is NULL");
    }
    *out = nullptr;
    if (config == nullptr) {
        return invalid(error, "wsrv_server_create: config is NULL");
    }
    return guarded(error, [&] {
        auto impl = wsrv::EmbeddedServer::create(config->value);
        if (!impl) {
            return report(impl.error(), error);
        }
        auto server = std::make_unique<wsrv_server>();
        server->impl = std::move(*impl);
        *out = server.release();
        return ok(error);
    });
}

wsrv_status wsrv_server_start(wsrv_server* server, wsrv_error* error) {
    if (server == nullptr) {
        return invalid(error, "wsrv_server_start: server is NULL");
    }
    return guarded(error, [&] {
        const wsrv::Result<void> started = server->impl->start();
        if (!started) {
            const wsrv_status status = wsrv::to_c_status(started.error().code);
            fill(error, status, server->impl->start_error());  // the reason, not just the kind
            return status;
        }
        return ok(error);
    });
}

uint16_t wsrv_server_port(const wsrv_server* server) {
    return server == nullptr ? 0 : server->impl->port();
}

int wsrv_server_event_fd(const wsrv_server* server) {
    return server == nullptr ? -1 : server->impl->event_fd();
}

wsrv_status wsrv_server_stop(wsrv_server* server) {
    if (server == nullptr) {
        return WSRV_ERR_INVALID_ARGUMENT;
    }
    return guarded(nullptr, [&] { return from_result(server->impl->stop(), nullptr); });
}

void wsrv_server_destroy(wsrv_server* server) {
    const std::unique_ptr<wsrv_server> owner{server};  // ~EmbeddedServer stops, waits and joins
}

// ---- events ------------------------------------------------------------------------------------

wsrv_status wsrv_poll(wsrv_server* server, wsrv_event* event, uint32_t timeout_ms) {
    if (server == nullptr || event == nullptr || event->struct_size < sizeof(wsrv_event)) {
        return WSRV_ERR_INVALID_ARGUMENT;
    }
    return guarded(nullptr, [&] {
        if (const wsrv::Result<void> same = server->impl->check_process(); !same) {
            return report(same.error(), nullptr);
        }
        std::optional<wsrv::HostEvent> next =
            server->impl->poll(std::chrono::milliseconds{timeout_ms});
        if (!next) {
            return WSRV_TIMEOUT;
        }
        auto owned = std::make_unique<wsrv::HostEvent>(std::move(*next));
        event->type = to_c_type(owned->type);
        event->conn = owned->conn;
        event->data = c_bytes(owned->data);
        event->len = owned->data.size();
        event->close_code = owned->close_code;
        event->clean = owned->clean ? 1 : 0;
        event->reserved = owned.release();
        return WSRV_OK;
    });
}

void wsrv_event_release(wsrv_event* event) {
    if (event == nullptr) {
        return;
    }
    const std::unique_ptr<wsrv::HostEvent> owner{static_cast<wsrv::HostEvent*>(event->reserved)};
    event->reserved = nullptr;
    event->data = nullptr;
    event->len = 0;
}

// ---- commands ----------------------------------------------------------------------------------

wsrv_status wsrv_send_text(wsrv_server* server, wsrv_conn_id conn, const char* text, size_t len) {
    if (server == nullptr || (text == nullptr && len != 0)) {
        return WSRV_ERR_INVALID_ARGUMENT;
    }
    return guarded(nullptr, [&] {
        const std::string_view view{text == nullptr ? "" : text, len};
        return from_result(server->impl->send_text(conn, view), nullptr);
    });
}

wsrv_status
wsrv_send_binary(wsrv_server* server, wsrv_conn_id conn, const uint8_t* data, size_t len) {
    if (server == nullptr || (data == nullptr && len != 0)) {
        return WSRV_ERR_INVALID_ARGUMENT;
    }
    return guarded(nullptr, [&] {
        const wsrv::ByteView view = std::as_bytes(std::span<const std::uint8_t>{data, len});
        return from_result(server->impl->send_binary(conn, view), nullptr);
    });
}

wsrv_status wsrv_close(  // NOLINT(readability-function-size): the published C signature
    wsrv_server* server,
    wsrv_conn_id conn,
    uint16_t code,
    const char* reason,
    size_t reason_len) {
    if (server == nullptr || (reason == nullptr && reason_len != 0)) {
        return WSRV_ERR_INVALID_ARGUMENT;
    }
    return guarded(nullptr, [&] {
        const std::string_view view{reason == nullptr ? "" : reason, reason_len};
        return from_result(server->impl->close(conn, static_cast<wsrv::CloseCode>(code), view),
                           nullptr);
    });
}

}  // extern "C"
