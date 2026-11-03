/* SPDX-License-Identifier: Apache-2.0 */
/*
 * A C-only consumer of libwsrv.so (ADR-0016 section 8c). It proves three things a C++ test cannot:
 *   - include/wsrv/wsrv.h compiles as pure C99 (-std=c99 -Wpedantic -Werror, no C++ types);
 *   - the shared library exports what the header promises and links without the C++ headers;
 *   - a server can be started, serve a real WebSocket handshake over a raw socket, echo a
 *     message through the host (poll, then send) and shut down, with only C calls.
 * Exit status 0 on success; on failure it prints the reason to stderr and returns 1.
 */
#define _POSIX_C_SOURCE 200809L

#include "wsrv/wsrv.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "consumer.c:%d: check failed: %s\n", __LINE__, #condition);   \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

/* Read exactly `want` bytes, giving up after 5 s without data. */
static int read_exact(int fd, unsigned char *buffer, size_t want) {
    size_t have = 0;
    while (have < want) {
        struct pollfd p;
        ssize_t n;
        p.fd = fd;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 5000) <= 0) {
            return 0;
        }
        n = recv(fd, buffer + have, want - have, 0);
        if (n <= 0) {
            return 0;
        }
        have += (size_t)n;
    }
    return 1;
}

/* Read the HTTP response head into `head` (NUL-terminated); 0 on failure. */
static int read_head(int fd, char *head, size_t room) {
    size_t used = 0;
    while (used + 1 < room) {
        if (!read_exact(fd, (unsigned char *)head + used, 1)) {
            return 0;
        }
        used += 1;
        head[used] = '\0';
        if (used >= 4 && strcmp(head + used - 4, "\r\n\r\n") == 0) {
            return 1;
        }
    }
    return 0;
}

static int connect_loopback(uint16_t port) {
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Poll until an event of `type` arrives (other types are released and ignored). */
static int expect_event(wsrv_server *server, wsrv_event_type type, wsrv_event *out) {
    int tries;
    for (tries = 0; tries < 50; ++tries) {
        wsrv_status status;
        memset(out, 0, sizeof *out);
        out->struct_size = (uint32_t)sizeof *out;
        status = wsrv_poll(server, out, 100);
        if (status == WSRV_OK) {
            if (out->type == type) {
                return 1;
            }
            wsrv_event_release(out);
        } else if (status != WSRV_TIMEOUT) {
            fprintf(stderr, "wsrv_poll: %s\n", wsrv_status_name(status));
            return 0;
        }
    }
    return 0;
}

/* Wait for a WSRV_EV_HANDSHAKE and check what the host is told (ADR-0018). */
static int hook_scenario(void) {
    static const char request[] =
        "GET /room/7?token=abc HTTP/1.1\r\n"
        "Host: chat.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: https://app.example.com\r\n"
        "Sec-WebSocket-Protocol: chat, v2\r\n"
        "Cookie: session=abc\r\n"
        "X-Not-Forwarded: nope\r\n"
        "\r\n";
    wsrv_config *config = wsrv_config_new();
    wsrv_server *server = NULL;
    wsrv_event event;
    wsrv_error error;
    char head[1024];
    int fd;

    CHECK(config != NULL);
    CHECK(wsrv_config_set(config, "allow_origin", "https://app.example.com", NULL) == WSRV_OK);
    CHECK(wsrv_config_set(config, "handshake_hook", "true", NULL) == WSRV_OK);
    CHECK(wsrv_config_set(config, "handshake_forward_header", "Cookie", NULL) == WSRV_OK);
    CHECK(wsrv_config_set(config, "handshake_decision_timeout_ms", "5000", NULL) == WSRV_OK);
    CHECK(wsrv_config_set(config, "shutdown_grace_ms", "1000", NULL) == WSRV_OK);
    CHECK(wsrv_server_create(config, &server, &error) == WSRV_OK);
    wsrv_config_free(config);
    CHECK(wsrv_server_start(server, &error) == WSRV_OK);

    /* Accepted, choosing an offered subprotocol. */
    fd = connect_loopback(wsrv_server_port(server));
    CHECK(fd >= 0);
    CHECK(send(fd, request, sizeof request - 1, 0) == (ssize_t)(sizeof request - 1));
    CHECK(expect_event(server, WSRV_EV_HANDSHAKE, &event));
    CHECK(event.conn != 0);
    CHECK(event.handshake != NULL);
    CHECK(event.handshake->path.len == 7 && memcmp(event.handshake->path.data, "/room/7", 7) == 0);
    CHECK(event.handshake->query.len == 9 && memcmp(event.handshake->query.data, "token=abc", 9) == 0);
    CHECK(event.handshake->host.len == 16);
    CHECK(event.handshake->origin.len == 23);
    CHECK(event.handshake->peer.len > 0);
    CHECK(event.handshake->offered_count == 2);
    CHECK(event.handshake->offered[1].len == 2 && memcmp(event.handshake->offered[1].data, "v2", 2) == 0);
    CHECK(event.handshake->header_count == 1); /* only Cookie was listed */
    CHECK(event.handshake->headers[0].value.len == 11);
    CHECK(memcmp(event.handshake->headers[0].value.data, "session=abc", 11) == 0);
    {
        const wsrv_conn_id conn = event.conn;
        wsrv_event_release(&event);
        CHECK(wsrv_handshake_accept(server, conn, "v2", 2) == WSRV_OK);
        CHECK(wsrv_handshake_accept(server, conn, NULL, 0) == WSRV_ERR_NO_SUCH_CONN); /* once */
        CHECK(read_head(fd, head, sizeof head));
        CHECK(strstr(head, "101") != NULL);
        CHECK(strstr(head, "Sec-WebSocket-Protocol: v2") != NULL);
        CHECK(expect_event(server, WSRV_EV_OPEN, &event)); /* same id as the question */
        CHECK(event.conn == conn);
        wsrv_event_release(&event);
    }
    close(fd);

    /* Refused: that status, and the host never sees an OPEN for it. */
    fd = connect_loopback(wsrv_server_port(server));
    CHECK(fd >= 0);
    CHECK(send(fd, request, sizeof request - 1, 0) == (ssize_t)(sizeof request - 1));
    CHECK(expect_event(server, WSRV_EV_HANDSHAKE, &event));
    {
        const wsrv_conn_id conn = event.conn;
        wsrv_event_release(&event);
        CHECK(wsrv_handshake_accept(server, conn, "not-offered", 11) == WSRV_ERR_INVALID_ARGUMENT);
        CHECK(wsrv_handshake_reject(server, conn, 302) == WSRV_ERR_INVALID_ARGUMENT);
        CHECK(wsrv_handshake_reject(server, conn, 401) == WSRV_OK);
        CHECK(read_head(fd, head, sizeof head));
        CHECK(strncmp(head, "HTTP/1.1 401 ", 13) == 0);
        CHECK(wsrv_handshake_reject(server, conn + 1000, 403) == WSRV_ERR_NO_SUCH_CONN);
    }
    close(fd);
    CHECK(wsrv_handshake_accept(NULL, 1, NULL, 0) == WSRV_ERR_INVALID_ARGUMENT);
    CHECK(wsrv_handshake_accept(server, 1, NULL, 3) == WSRV_ERR_INVALID_ARGUMENT);

    CHECK(wsrv_server_stop(server) == WSRV_OK);
    wsrv_server_destroy(server);
    return 0;
}

int main(void) {
    static const char request[] =
        "GET /chat HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Origin: https://app.example.com\r\n"
        "\r\n";
    /* A masked client text frame "hi": FIN+text, MASK+len 2, key 1 2 3 4, payload xor key. */
    static const unsigned char client_text[] = {0x81, 0x82, 1, 2, 3, 4, 'h' ^ 1, 'i' ^ 2};
    wsrv_error error;
    wsrv_config *config;
    wsrv_server *server = NULL;
    wsrv_event event;
    unsigned char frame[16];
    char head[1024];
    int fd;

    CHECK((wsrv_abi_version() >> 16) == (WSRV_ABI_VERSION >> 16));
    CHECK(strcmp(wsrv_status_name(WSRV_ERR_NO_SUCH_CONN), "WSRV_ERR_NO_SUCH_CONN") == 0);
    {   /* every status has a name; a value this version does not know does not crash */
        int code;
        for (code = (int)WSRV_OK; code <= (int)WSRV_ERR_INTERNAL; ++code) {
            CHECK(strncmp(wsrv_status_name(code), "WSRV_", 5) == 0);
            CHECK(strcmp(wsrv_status_name(code), "WSRV_ERR_UNKNOWN") != 0);
        }
        CHECK(strcmp(wsrv_status_name(9999), "WSRV_ERR_UNKNOWN") == 0);
    }

    config = wsrv_config_new();
    CHECK(config != NULL);
    CHECK(wsrv_config_set(config, "allow_origin", "https://app.example.com", &error) == WSRV_OK);
    CHECK(wsrv_config_set(config, "shutdown_grace_ms", "1000", &error) == WSRV_OK);
    /* A bad value is refused with a reason and changes nothing. */
    CHECK(wsrv_config_set(config, "threads", "many", &error) == WSRV_ERR_INVALID_CONFIG);
    CHECK(error.message[0] != '\0');
    CHECK(wsrv_config_set(config, "no_such_key", "1", NULL) == WSRV_ERR_INVALID_CONFIG);

    CHECK(wsrv_server_create(config, &server, &error) == WSRV_OK);
    wsrv_config_free(config); /* the server keeps its own copy */
    CHECK(server != NULL);
    CHECK(wsrv_server_event_fd(server) >= 0);
    CHECK(wsrv_server_port(server) == 0);
    CHECK(wsrv_server_start(server, &error) == WSRV_OK);
    CHECK(wsrv_server_port(server) != 0);
    /* Starting twice is a caller bug, reported and harmless. */
    CHECK(wsrv_server_start(server, NULL) == WSRV_ERR_INVALID_ARGUMENT);

    fd = connect_loopback(wsrv_server_port(server));
    CHECK(fd >= 0);
    CHECK(send(fd, request, sizeof request - 1, 0) == (ssize_t)(sizeof request - 1));
    CHECK(read_head(fd, head, sizeof head));
    CHECK(strstr(head, "101") != NULL);
    CHECK(strstr(head, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL); /* RFC 6455 section 1.3 */

    CHECK(expect_event(server, WSRV_EV_OPEN, &event));
    CHECK(event.conn != 0);
    CHECK(event.len > 0 && event.data != NULL);
    {
        const wsrv_conn_id conn = event.conn;
        wsrv_event_release(&event);

        CHECK(send(fd, client_text, sizeof client_text, 0) == (ssize_t)sizeof client_text);
        CHECK(expect_event(server, WSRV_EV_TEXT, &event));
        CHECK(event.conn == conn && event.len == 2 && memcmp(event.data, "hi", 2) == 0);
        CHECK(wsrv_send_text(server, event.conn, (const char *)event.data, event.len) == WSRV_OK);
        wsrv_event_release(&event);

        /* The echo: an unmasked server frame, FIN+text, length 2. */
        CHECK(read_exact(fd, frame, 4));
        CHECK(frame[0] == 0x81 && frame[1] == 2 && frame[2] == 'h' && frame[3] == 'i');

        /* An invalid text and an unknown connection are refused without sending anything. */
        CHECK(wsrv_send_text(server, conn, "\xff\xfe", 2) == WSRV_ERR_INVALID_UTF8);
        CHECK(wsrv_send_text(server, conn + 1000, "x", 1) == WSRV_ERR_NO_SUCH_CONN);

        /* The peer leaves: the host sees exactly one CLOSE for this connection. */
        close(fd);
        CHECK(expect_event(server, WSRV_EV_CLOSE, &event));
        CHECK(event.conn == conn);
        wsrv_event_release(&event);
        wsrv_event_release(&event); /* releasing twice is safe */
    }

    CHECK(wsrv_server_stop(server) == WSRV_OK);
    CHECK(wsrv_server_stop(server) == WSRV_OK); /* idempotent */
    wsrv_server_destroy(server);
    wsrv_server_destroy(NULL);
    return hook_scenario();
}
