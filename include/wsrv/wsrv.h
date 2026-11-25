/* SPDX-License-Identifier: Apache-2.0 */
/*
 * wsrv C API, version 0.2 (ABI major 0: not yet frozen; frozen at 1.0, additive only after).
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

/* (major << 16) | minor. Compare the major with the one you compiled against. From 1.0 the major
 * changes only with wsrv 2.0 and a minor only adds (ADR-0016 amendment A4). */
#define WSRV_ABI_VERSION ((1u << 16) | 0u)
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
    WSRV_ERR_INTERNAL = 11,         /* a bug; please report it */
    WSRV_ERR_LIMIT = 12             /* a broker bound refused a subscription (0.5; wsrvd reports it
                                       over IPC 0.3); retrying does not help until someone leaves */
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
 *   max_frame_payload    (0.4) largest single frame in bytes, 1 to 67108864; default 1 MiB.
 *                        Raise it together with max_message_size, only for clients you trust:
 *                        it is the biggest buffer one connection can ask the server to hold.
 *   max_fragments        (0.4) most frames one message may be split into, 1 to 16777216; default
 *                        1024. More fragments cost bookkeeping, not memory.
 *   max_data_frames_per_second  (0.4) data frames one connection may send per second before it is
 *                        closed with 1008, 1 to 10000000; default 10000. This is the flood guard:
 *                        there is no "off". Raise it only for a trusted batch client.
 *   max_connections      simultaneous connections
 *   max_connections_per_ip  simultaneous connections from one client address (IPv4, or the /64
 *                        of an IPv6 address), default 100; 0 = no limit. Over it: HTTP 429.
 *   max_new_connections_per_ip_per_second  new connections per second from one client address,
 *                        default 50; 0 = no limit. Over it: HTTP 429. Behind a reverse proxy every
 *                        user is one address: raise or switch off.
 *   admin_port           0..65535: serve /metrics (Prometheus text), /healthz and /readyz over
 *                        plain HTTP on this port. Off unless set; 0 lets the kernel choose. There
 *                        is no authentication: keep it on loopback or behind a network policy.
 *   admin_host           IP literal for the admin port, default "127.0.0.1"
 *   shutdown_grace_ms    graceful shutdown deadline
 *   event_queue_events   most events waiting for the host. Every live connection holds one slot
 *                        for its Close event, so this must exceed max_connections. Unset, it is
 *                        max_connections + 4096; set by hand, it is used as given (a smaller
 *                        value caps the connections the server can hold at that value)
 *   event_queue_bytes    most payload bytes waiting for the host. Unset, it is max_connections
 *                        times the bytes reserved per Close plus 8 MiB; set by hand, as given
 *   handshake_hook       "true"/"false"/"1"/"0", default false. When true, every valid upgrade
 *                        waits for the host: a WSRV_EV_HANDSHAKE event, answered with
 *                        wsrv_handshake_accept() or wsrv_handshake_reject(). Unanswered upgrades
 *                        are refused with 503 (fail closed). See "handshake decisions" below.
 *   handshake_forward_header  a request header name (for example "Cookie") to include in the
 *                        event; repeatable. Cookie and Authorization are NOT sent unless listed.
 *   trusted_proxy       an address or CIDR of a reverse proxy whose forwarded-address header is
 *                        believed; repeatable, at most 64, default none (feature off). Behind such
 *                        a proxy the per-IP limits and the peer address in events are the client's.
 *   client_ip_header     the header naming the client, default "X-Forwarded-For" (read as a list,
 *                        rightmost untrusted entry); any other name is read as one address.
 *   handshake_decision_timeout_ms  how long the host has to answer, default 3000 (> 0)
 *   max_pending_handshakes  most upgrades waiting at once, default 256 (> 0); more get 503
 *   max_pending_send_bytes  per loop, the most payload bytes of wsrv_send_* calls waiting for the
 *                        loop, default 67108864 (1 .. 1073741824); over it a send returns
 *                        WSRV_ERR_BACKPRESSURE (wait and retry), a message larger than the whole
 *                        budget WSRV_ERR_INVALID_ARGUMENT
 *   tls_cert             path of the server certificate chain (PEM, certificate first)
 *   tls_key              path of its private key (PEM, not readable by "others"; no passphrase)
 *                        Setting either asks for wss://: connections are then TLS only (1.2 and
 *                        1.3, hardened profile). The files are read by wsrv_server_start(); a
 *                        missing or unusable file, only one of the two keys, or a build without
 *                        TLS makes the start fail with WSRV_ERR_INVALID_CONFIG and the reason in
 *                        `error`. It never falls back to plain text.
 *   tls_sni              "name=cert_file:key_file"; repeatable, at most 64. Serve that certificate
 *                        to clients that ask for `name` (case-blind) in the TLS server_name
 *                        extension; name is a host name or one leading wildcard label
 *                        ("*.example.com": exactly one label, never "*.com"). Exact names win over
 *                        wildcards. Clients that send no name, an unknown name or a malformed one
 *                        get the tls_cert certificate (never an error). Needs tls_cert and tls_key.
 *                        The paths must not contain ':'. Reloaded with the rest.
 *   permessage_deflate   "true"/"false"/"1"/"0", default false. Offer permessage-deflate (RFC 7692)
 *                        to clients that ask for it. Your handlers still see whole, inflated
 *                        messages and send plain ones; compression happens inside the server.
 *                        Costs CPU and memory per connection (see docs/OPERATIONS.md) and mixing
 *                        secrets with attacker-controlled text in one message is risky
 *                        (CRIME/BREACH class), so it is opt-in. A build without zlib makes the
 *                        start fail with WSRV_ERR_INVALID_CONFIG; it never falls back silently.
 *   deflate_server_max_window_bits  9..15, default 12: window of the server's compressor.
 *   deflate_server_no_context_takeover  boolean, default true: reset the compressor after each
 *                        message (less memory and CPU state, slightly worse ratio).
 *   deflate_level        1..9, default 6: zlib level (CPU against ratio).
 *   deflate_min_size     bytes, default 128: shorter outgoing messages are sent uncompressed.
 *   deflate_max_ratio    1..1000, default 100: an incoming message may inflate to at most this
 *                        many times its compressed size (plus a small allowance), else the
 *                        connection is closed with 1009. The deflate_* keys are only used when
 *                        permessage_deflate is true (any order); a value outside its range makes
 *                        wsrv_server_start() fail with WSRV_ERR_INVALID_CONFIG.
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

/* A descriptor that becomes readable when events wait: add it to your poll/epoll/select loop, then
 * call wsrv_poll(server, &ev, 0) until it returns WSRV_TIMEOUT. Do not read or close it. The
 * server may pay a message's wake-up at the end of the I/O loop iteration that received it, so
 * the descriptor can lag wsrv_poll() by that much; wsrv_poll() returns such an event already, and
 * the descriptor never stays silent while an event waits (it is the wake-up that is batched, not
 * the event). Do not use the descriptor's readability as a test that the queue is empty. */
WSRV_API int wsrv_server_event_fd(const wsrv_server *server);

/* Re-read tls_cert and tls_key and use them for connections accepted from now on (connections
 * already open keep their certificate). Call it after a successful start, for example when the
 * certificate was renewed; any thread. If the new files are unusable (missing, expired, key does
 * not match, key readable by everyone) nothing changes, the server keeps serving the old
 * certificate, and WSRV_ERR_INVALID_CONFIG comes back with the reason in `error`.
 * WSRV_ERR_INVALID_ARGUMENT if the server was not configured for TLS. */
WSRV_API wsrv_status wsrv_server_reload_tls(wsrv_server *server, wsrv_error *error);

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
    WSRV_EV_ERROR = 5,  /* data = a short error name; informational, may be dropped */
    WSRV_EV_HANDSHAKE = 6 /* (0.2) an upgrade waits for your answer; see `handshake` below */
} wsrv_event_type;

/* A byte string that is NOT NUL-terminated: use `len`. Never NULL `data`, even when len is 0. */
typedef struct wsrv_str {
    const char *data;
    size_t len;
} wsrv_str;

typedef struct wsrv_header {
    wsrv_str name; /* spelled as in handshake_forward_header */
    wsrv_str value;
} wsrv_header;

/* What the client asked for (WSRV_EV_HANDSHAKE). All of it belongs to the event and is valid until
 * wsrv_event_release(). Values come straight from the client: validate them, never trust them. */
typedef struct wsrv_handshake {
    wsrv_str peer;     /* "host:port" */
    wsrv_str path;     /* "/room/7" */
    wsrv_str query;    /* "token=abc" (no '?'), empty if none */
    wsrv_str origin;   /* the Origin header, empty if none (it already passed allow_origin) */
    wsrv_str host;     /* the Host header */
    wsrv_str subprotocol;            /* what the server would choose; may be empty */
    const wsrv_str *offered;         /* every subprotocol the client offered */
    size_t offered_count;
    const wsrv_header *headers;      /* the handshake_forward_header ones, every value, in order */
    size_t header_count;
} wsrv_handshake;

/* For one connection the order is always: OPEN, then messages, then exactly one CLOSE. A
 * HANDSHAKE comes before the OPEN of an accepted upgrade (same conn id); a refused upgrade
 * produces no OPEN and no CLOSE. */
typedef struct wsrv_event {
    uint32_t struct_size; /* set to sizeof(wsrv_event) BEFORE calling wsrv_poll */
    wsrv_event_type type;
    wsrv_conn_id conn;
    const uint8_t *data; /* owned by the library until wsrv_event_release(); never NULL */
    size_t len;
    uint16_t close_code; /* CLOSE: the peer's code, or 1006 when the connection just vanished */
    int clean;           /* CLOSE: the closing handshake completed */
    void *reserved;      /* private */
    const wsrv_handshake *handshake; /* (0.2) HANDSHAKE: the request, else NULL */
} wsrv_event;

/* Wait up to timeout_ms for the next event (0 = only look). WSRV_OK fills *event, which you must
 * pass to wsrv_event_release() when you have copied what you need. WSRV_TIMEOUT: nothing arrived.
 * One thread at a time may call this per server. On WSRV_TIMEOUT (or any error) *event is left
 * untouched: release only an event that was filled by WSRV_OK, or one you zeroed yourself. */
WSRV_API wsrv_status wsrv_poll(wsrv_server *server, wsrv_event *event, uint32_t timeout_ms);
WSRV_API void wsrv_event_release(wsrv_event *event); /* safe on a released or zeroed event */

/* ---- handshake decisions (0.2): any thread, by the conn id of a WSRV_EV_HANDSHAKE ---------- */

/* Let the upgrade in. `subprotocol` (NULL or len 0 = none) must be one the client offered; the
 * server then answers 101 with it. WSRV_ERR_INVALID_ARGUMENT: not offered (nothing changes, the
 * wait goes on, you may answer again). WSRV_ERR_NO_SUCH_CONN: already answered, or the server
 * gave up (timeout, the client left, shutdown): answer once, within the timeout. OK means handed
 * to the connection's loop, as for the commands below. */
WSRV_API wsrv_status wsrv_handshake_accept(wsrv_server *server,
                                           wsrv_conn_id conn,
                                           const char *subprotocol,
                                           size_t subprotocol_len);

/* Refuse the upgrade with an HTTP status: 400, 401, 403, 404, 429, 500 or 503 (anything else is
 * WSRV_ERR_INVALID_ARGUMENT and the wait goes on). The reason text is fixed by the server: nothing
 * you pass reaches the client. 429 and 503 carry "Retry-After: 5". */
WSRV_API wsrv_status wsrv_handshake_reject(wsrv_server *server, wsrv_conn_id conn, int status);

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
