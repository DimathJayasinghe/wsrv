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
#include <vector>

#include "capi/c_status.hpp"
#include "capi/embedded_config.hpp"
#include "capi/embedded_server.hpp"
#include "capi/handshake_blob.hpp"

struct wsrv_config {
    wsrv::EmbeddedConfig value;
};

struct wsrv_server {
    std::unique_ptr<wsrv::EmbeddedServer> impl;
};

namespace {

/// Everything one polled event owns: the queued event and, for a handshake, the decoded view that
/// the C structs point into (the strings point into `event.data`, which never moves again).
/// `wsrv_event.reserved` is a pointer to one of these, freed by wsrv_event_release().
struct EventBox {
    wsrv::HostEvent event;
    std::vector<wsrv_str> offered;
    std::vector<wsrv_header> headers;
    wsrv_handshake handshake{};
};

wsrv_str c_str(std::string_view text) noexcept {
    // An empty view may carry a null pointer; the contract says data is never NULL.
    return wsrv_str{text.empty() ? "" : text.data(), text.size()};
}

/// Fill box.handshake from box.event.data. @return false if the blob is not well formed (cannot
/// happen for a blob our own encoder made; checked because it crosses a queue).
bool describe_handshake(EventBox& box) {
    const auto view = wsrv::decode_handshake(wsrv::ByteView{box.event.data});
    if (!view) {
        return false;
    }
    for (const std::string_view item : view->offered) {
        box.offered.push_back(c_str(item));
    }
    for (const auto& [name, value] : view->headers) {
        box.headers.push_back(wsrv_header{c_str(name), c_str(value)});
    }
    box.handshake = wsrv_handshake{c_str(view->peer),
                                   c_str(view->path),
                                   c_str(view->query),
                                   c_str(view->origin),
                                   c_str(view->host),
                                   c_str(view->subprotocol),
                                   box.offered.data(),
                                   box.offered.size(),
                                   box.headers.data(),
                                   box.headers.size()};
    return true;
}

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
    case wsrv::HostEventType::Handshake:
        return WSRV_EV_HANDSHAKE;
    }
    return WSRV_EV_ERROR;
}

/// Copy the box's event into the caller's struct. @return false if a handshake blob is malformed.
bool publish(EventBox& box, wsrv_event& out) {
    const bool is_handshake = box.event.type == wsrv::HostEventType::Handshake;
    if (is_handshake && !describe_handshake(box)) {
        return false;
    }
    out.type = to_c_type(box.event.type);
    out.conn = box.event.conn;
    // The blob is an implementation detail: a handshake exposes only `handshake`.
    out.data = is_handshake ? kNoBytes.data() : c_bytes(box.event.data);
    out.len = is_handshake ? 0 : box.event.data.size();
    out.close_code = box.event.close_code;
    out.clean = box.event.clean ? 1 : 0;
    out.handshake = is_handshake ? &box.handshake : nullptr;
    return true;
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
    case WSRV_ERR_LIMIT:
        return "WSRV_ERR_LIMIT";
    default:
        break;  // a number this library does not know: a newer one may have added it
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

wsrv_status wsrv_server_reload_tls(wsrv_server* server, wsrv_error* error) {
    if (server == nullptr) {
        return invalid(error, "wsrv_server_reload_tls: server is NULL");
    }
    return guarded(error, [&] {
        const wsrv::Result<void> reloaded = server->impl->reload_tls();
        if (!reloaded) {
            const wsrv_status status = wsrv::to_c_status(reloaded.error().code);
            const std::string detail = server->impl->tls_detail();
            fill(error,
                 status,
                 detail.empty() ? std::string_view{reloaded.error().context} : detail);
            return status;
        }
        return ok(error);
    });
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
        auto owned = std::make_unique<EventBox>();
        owned->event = std::move(*next);
        if (!publish(*owned, *event)) {
            // Unreachable by construction; fail closed rather than leave the client waiting.
            static_cast<void>(server->impl->reject_handshake(owned->event.conn, 503).has_value());
            return WSRV_ERR_INTERNAL;
        }
        event->reserved = owned.release();
        return WSRV_OK;
    });
}

void wsrv_event_release(wsrv_event* event) {
    if (event == nullptr) {
        return;
    }
    const std::unique_ptr<EventBox> owner{static_cast<EventBox*>(event->reserved)};
    event->reserved = nullptr;
    event->data = nullptr;
    event->len = 0;
    event->handshake = nullptr;
}

// ---- handshake decisions -----------------------------------------------------------------------

wsrv_status wsrv_handshake_accept(wsrv_server* server,
                                  wsrv_conn_id conn,
                                  const char* subprotocol,
                                  size_t subprotocol_len) {
    if (server == nullptr || (subprotocol == nullptr && subprotocol_len != 0)) {
        return WSRV_ERR_INVALID_ARGUMENT;
    }
    return guarded(nullptr, [&] {
        std::optional<std::string_view> chosen;
        if (subprotocol_len != 0) {
            chosen = std::string_view{subprotocol, subprotocol_len};
        }
        return from_result(server->impl->accept_handshake(conn, chosen), nullptr);
    });
}

wsrv_status wsrv_handshake_reject(wsrv_server* server, wsrv_conn_id conn, int status) {
    if (server == nullptr) {
        return WSRV_ERR_INVALID_ARGUMENT;
    }
    return guarded(nullptr, [&] {
        return from_result(server->impl->reject_handshake(conn, status), nullptr);
    });
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
