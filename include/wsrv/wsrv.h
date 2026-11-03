/* SPDX-License-Identifier: Apache-2.0 */
/*
 * wsrv C API, version 0.1 (ABI major 0: not yet frozen; frozen at 1.0, additive only after).
 *
 * An embedded WebSocket server (RFC 6455): the library runs its own I/O thread(s); the host
 * (your language runtime) never runs on them and they never call into it. The host PULLS events
 * with wsrv_poll() (optionally waiting on wsrv_server_event_fd() in its own event loop) and
 * pushes commands (send, close) by connection id from any thread.
 *
 * Everything that is easy to get wrong is done here, once: the protocol, UTF-8 checking, limits,
 * backpressure, exactly one CLOSE event for every OPEN event, safe sends to connections that
 * have gone, and shutdown. A language binding only converts types.
 *
 * Rules:
 *  - Pure C99. No function throws or longjmps; every failure is a wsrv_status.
 *  - Text is passed as pointer + length (no NUL terminator needed or assumed).
 *  - Thread-safety is stated per function: "any thread" or "one poller at a time".
 *  - The library installs no signal handlers. Create the server AFTER fork(); a server used from
 *    a forked child fails with WSRV_ERR_WRONG_PROCESS.
 */
#ifndef WSRV_WSRV_H
#define WSRV_WSRV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define WSRV_API __attribute__((visibility("default")))
#else
#define WSRV_API
#endif

/* (major << 16) | minor. Compare the major with the one you compiled against. */
#define WSRV_ABI_VERSION ((0u << 16) | 1u)
WSRV_API uint32_t wsrv_abi_version(void);

typedef enum wsrv_status {
    WSRV_OK = 0,
    WSRV_TIMEOUT = 1,               /* wsrv_poll: no event arrived in time (not an error) */
    WSRV_ERR_INVALID_ARGUMENT = 2,  /* NULL pointer, wrong state (for example started twice) */
    WSRV_ERR_INVALID_CONFIG = 3,    /* unknown key, malformed value, or unusable settings */
    WSRV_ERR_ADDRESS_IN_USE = 4,    /* wsrv_server_start: the port is taken */
    WSRV_ERR_NO_SUCH_CONN = 5,      /* the connection id is unknown or has ended */
    WSRV_ERR_BACKPRESSURE = 6,      /* a queue is full: try again later */
    WSRV_ERR_INVALID_UTF8 = 7,      /* text is not valid UTF-8; nothing was sent */
    WSRV_ERR_WRONG_PROCESS = 8,     /* used from a forked child of the creating process */
    WSRV_ERR_RESOURCE = 9,          /* out of memory or descriptors */
    WSRV_ERR_IO = 10,               /* an operating-system failure */
    WSRV_ERR_INTERNAL = 11          /* a bug; please report it */
} wsrv_status;

/* A short constant name such as "WSRV_ERR_NO_SUCH_CONN", or "WSRV_ERR_UNKNOWN" for a number this
 * library does not know (a newer library may return codes an older header lacks). Never NULL. The
 * parameter is an int, not a wsrv_status: an enum may only hold its declared values, so passing an
 * undeclared number as an enum would be undefined behaviour. Any thread. */
WSRV_API const char *wsrv_status_name(int status);

/* Filled on failure by the functions that take one (pass NULL to ignore). The message is for
 * people and logs, NUL-terminated, truncated to fit, never to be parsed. */
typedef struct wsrv_error {
    wsrv_status status;
    char message[128];
} wsrv_error;

typedef uint64_t wsrv_conn_id; /* process-unique, never reused; 0 is never a connection */

/* ---- configuration ----------------------------------------------------------------------- */

typedef struct wsrv_config wsrv_config;

/* A new configuration with safe defaults (loopback, port 0, no Origin allowed). NULL if out of
 * memory. Free with wsrv_config_free(). */
WSRV_API wsrv_config *wsrv_config_new(void);
WSRV_API void wsrv_config_free(wsrv_config *config); /* NULL is fine */

/* Set one text setting. An unknown key or a malformed value fails with WSRV_ERR_INVALID_CONFIG
 * and changes nothing. Keys (never removed; new ones are added over time):
 *   listen_host          IP literal, default "127.0.0.1" ("0.0.0.0" to accept from outside)
 *   listen_port          0..65535, default 0 (the kernel chooses; read it with wsrv_server_port)
 *   threads              event loops, default 1
 *   allow_origin         an allowed browser Origin, e.g. "https://app.example.com"; repeatable.
 *                        Browsers are refused unless their Origin is listed.
 *   allow_missing_origin "true"/"false"/"1"/"0": accept clients that send no Origin header
 *   max_message_size     largest message in bytes, both directions
 *   max_connections      simultaneous connections
 *   shutdown_grace_ms    graceful shutdown deadline
 *   event_queue_events   most events waiting for the host
 *   event_queue_bytes    most payload bytes waiting for the host
 * Any thread, but not concurrently with another call on the same config. */
WSRV_API wsrv_status wsrv_config_set(wsrv_config *config,
                                     const char *key,
                                     const char *value,
                                     wsrv_error *error);

/* ---- server lifecycle -------------------------------------------------------------------- */

typedef struct wsrv_server wsrv_server;

/* Create a server from `config` (which may be freed afterwards). Nothing is bound or started yet.
 * On success *out is the server; on failure *out is NULL. */
WSRV_API wsrv_status
wsrv_server_create(const wsrv_config *config, wsrv_server **out, wsrv_error *error);

/* Bind and start the I/O threads. Returns when the server is listening or has failed (for example
 * WSRV_ERR_ADDRESS_IN_USE, WSRV_ERR_INVALID_CONFIG with the reason in `error`). Once. */
WSRV_API wsrv_status wsrv_server_start(wsrv_server *server, wsrv_error *error);

/* The port actually bound; 0 before a successful start. Any thread. */
WSRV_API uint16_t wsrv_server_port(const wsrv_server *server);

/* A descriptor that is readable while events wait: add it to your poll/epoll/select loop, then
 * call wsrv_poll(server, &ev, 0) until it returns WSRV_TIMEOUT. Do not read or close it. */
WSRV_API int wsrv_server_event_fd(const wsrv_server *server);

/* Begin a graceful shutdown: connections get Close 1001, then the server ends after
 * shutdown_grace_ms. Any thread, idempotent, fine before start or after the end. Keep polling to
 * see the remaining CLOSE events. */
WSRV_API wsrv_status wsrv_server_stop(wsrv_server *server);

/* Stop if needed, wait, free everything. After this the pointer and every wsrv_event not yet
 * released are invalid. NULL is fine. Call it from one thread, with no other call in flight. */
WSRV_API void wsrv_server_destroy(wsrv_server *server);

/* ---- events ------------------------------------------------------------------------------ */

typedef enum wsrv_event_type {
    WSRV_EV_OPEN = 1,   /* data = peer address "host:port" */
    WSRV_EV_TEXT = 2,   /* data = a complete message, valid UTF-8 */
    WSRV_EV_BINARY = 3, /* data = a complete message */
    WSRV_EV_CLOSE = 4,  /* data = the close reason; close_code and clean are set */
    WSRV_EV_ERROR = 5   /* data = a short error name; informational, may be dropped */
} wsrv_event_type;

/* For one connection the order is always: OPEN, then messages, then exactly one CLOSE. */
typedef struct wsrv_event {
    uint32_t struct_size; /* set to sizeof(wsrv_event) BEFORE calling wsrv_poll */
    wsrv_event_type type;
    wsrv_conn_id conn;
    const uint8_t *data; /* owned by the library until wsrv_event_release(); never NULL */
    size_t len;
    uint16_t close_code; /* CLOSE: the peer's code, or 1006 when the connection just vanished */
    int clean;           /* CLOSE: the closing handshake completed */
    void *reserved;      /* private */
} wsrv_event;

/* Wait up to timeout_ms for the next event (0 = only look). WSRV_OK fills *event, which you must
 * pass to wsrv_event_release() when you have copied what you need. WSRV_TIMEOUT: nothing arrived.
 * One thread at a time may call this per server. */
WSRV_API wsrv_status wsrv_poll(wsrv_server *server, wsrv_event *event, uint32_t timeout_ms);
WSRV_API void wsrv_event_release(wsrv_event *event); /* safe on a released or zeroed event */

/* ---- commands: any thread, by connection id ---------------------------------------------- */

/* "Accepted" means handed to the connection's loop, not delivered: a connection can still end
 * before it is written. WSRV_ERR_NO_SUCH_CONN: unknown or already ended. Text must be valid UTF-8
 * (WSRV_ERR_INVALID_UTF8 otherwise, nothing sent). */
WSRV_API wsrv_status
wsrv_send_text(wsrv_server *server, wsrv_conn_id conn, const char *text, size_t len);
WSRV_API wsrv_status
wsrv_send_binary(wsrv_server *server, wsrv_conn_id conn, const uint8_t *data, size_t len);

/* Start the closing handshake. `code` must be sendable (1000-1003, 1007-1014, 3000-4999), the
 * reason is cut to 123 bytes. */
WSRV_API wsrv_status wsrv_close(wsrv_server *server,
                                wsrv_conn_id conn,
                                uint16_t code,
                                const char *reason,
                                size_t reason_len);

#ifdef __cplusplus
}
#endif

#endif /* WSRV_WSRV_H */
