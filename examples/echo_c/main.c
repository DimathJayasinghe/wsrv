/* SPDX-License-Identifier: Apache-2.0 */
/*
 * echo_c: a WebSocket echo server written in plain C99 against include/wsrv/wsrv.h, and nothing else.
 *
 * It is the C twin of examples/echo (C++), and the proof for M11's exit criterion: everything the
 * Autobahn conformance suite needs can be done through the C ABI. The protocol, UTF-8 checking,
 * limits, ping/pong and the closing handshake all happen inside libwsrv; this file only decides
 * "send back what came in".
 *
 * The shape every C host has (see docs/BINDINGS.md):
 *     config -> server -> start -> loop { wsrv_poll -> handle event -> wsrv_event_release } -> destroy
 *
 *   echo_c [--host H] [--port P] [--allow-missing-origin] [--tls-cert FILE --tls-key FILE]
 *          [--deflate] [--autobahn]
 *
 * --autobahn raises size, rate and per-IP limits for the conformance suite only (the numbers and
 * the cases that need them: tests/autobahn/EXCLUDED.md). None of it is a library default.
 */
#define _POSIX_C_SOURCE 200809L

#include "wsrv/wsrv.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The one global in this program. A signal handler can reach nothing but globals, and
 * sig_atomic_t is the only type the C standard lets a handler write. It carries one bit: "stop". */
static volatile sig_atomic_t g_stop_requested = 0;

static void on_signal(int signal_number) {
    (void)signal_number;
    g_stop_requested = 1;
}

typedef struct options {
    const char *host;
    const char *port;
    const char *tls_cert;
    const char *tls_key;
    int allow_missing_origin;
    int deflate;
    int autobahn;
} options;

static void usage(void) {
    fputs("usage: echo_c [--host H] [--port P] [--allow-missing-origin] [--tls-cert FILE "
          "--tls-key FILE] [--deflate] [--autobahn]\n",
          stderr);
}

/* 1 on success. Values are passed to the library as text and validated there (a bad port is
 * rejected by wsrv_config_set), so this file never parses a number itself. */
static int parse_args(int argc, char **argv, options *out) {
    int i;
    memset(out, 0, sizeof *out);
    out->host = "127.0.0.1";
    out->port = "9001";
    for (i = 1; i < argc; ++i) {
        const int has_value = i + 1 < argc;
        if (strcmp(argv[i], "--host") == 0 && has_value) {
            out->host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && has_value) {
            out->port = argv[++i];
        } else if (strcmp(argv[i], "--tls-cert") == 0 && has_value) {
            out->tls_cert = argv[++i];
        } else if (strcmp(argv[i], "--tls-key") == 0 && has_value) {
            out->tls_key = argv[++i];
        } else if (strcmp(argv[i], "--allow-missing-origin") == 0) {
            out->allow_missing_origin = 1;
        } else if (strcmp(argv[i], "--deflate") == 0) {
            out->deflate = 1;
        } else if (strcmp(argv[i], "--autobahn") == 0) {
            out->autobahn = 1;
        } else {
            usage();
            return 0;
        }
    }
    return 1;
}

static int set(wsrv_config *config, const char *key, const char *value) {
    wsrv_error error;
    if (wsrv_config_set(config, key, value, &error) != WSRV_OK) {
        fprintf(stderr, "echo_c: %s=%s: %s\n", key, value, error.message);
        return 0;
    }
    return 1;
}

/* The limits the conformance suite needs and the library keeps low by default: one frame of up to
 * 16 MiB, a 4 MiB message in 65 536 fragments, bursts of small frames, and one client address
 * opening hundreds of connections. event_queue_bytes must hold a whole message too: the host sees
 * messages whole, so a queue smaller than max_message_size could never deliver the biggest one. */
static int raise_limits_for_autobahn(wsrv_config *config) {
    return set(config, "max_message_size", "20971520") &&
           set(config, "max_frame_payload", "20971520") &&
           set(config, "max_fragments", "1048576") &&
           set(config, "max_data_frames_per_second", "2000000") &&
           set(config, "event_queue_bytes", "67108864") &&
           set(config, "max_connections_per_ip", "0") &&
           set(config, "max_new_connections_per_ip_per_second", "0");
}

static int configure(wsrv_config *config, const options *opts) {
    if (!set(config, "listen_host", opts->host) || !set(config, "listen_port", opts->port)) {
        return 0;
    }
    if (opts->allow_missing_origin && !set(config, "allow_missing_origin", "true")) {
        return 0;
    }
    if (opts->tls_cert != NULL && !set(config, "tls_cert", opts->tls_cert)) {
        return 0;
    }
    if (opts->tls_key != NULL && !set(config, "tls_key", opts->tls_key)) {
        return 0;
    }
    if (opts->deflate && !set(config, "permessage_deflate", "true")) {
        return 0;
    }
    return !opts->autobahn || raise_limits_for_autobahn(config);
}

static void sleep_one_millisecond(void) {
    struct timespec pause;
    pause.tv_sec = 0;
    pause.tv_nsec = 1000000L;
    nanosleep(&pause, NULL);
}

/* wsrv_send_* says "accepted" or WSRV_ERR_BACKPRESSURE ("a queue is full: try again later").
 * An echo must neither lose a message nor reorder two, so on BACKPRESSURE this waits a moment and
 * tries the same message again (nothing else is sent meanwhile: this thread is the only sender).
 * After about a second of a full queue the peer is not reading: close it with 1013 ("try again
 * later", RFC 6455 section 7.4.1 registry) instead of waiting forever. Any other failure means the
 * connection is gone (WSRV_ERR_NO_SUCH_CONN): there is nobody to echo to. */
static void echo_message(wsrv_server *server, const wsrv_event *event) {
    int attempts;
    for (attempts = 0; attempts < 1000; ++attempts) {
        wsrv_status status;
        if (event->type == WSRV_EV_TEXT) {
            status = wsrv_send_text(server, event->conn, (const char *)event->data, event->len);
        } else {
            status = wsrv_send_binary(server, event->conn, event->data, event->len);
        }
        if (status != WSRV_ERR_BACKPRESSURE) {
            return;
        }
        sleep_one_millisecond();
    }
    (void)wsrv_close(server, event->conn, 1013, "slow reader", 11);
}

/* Poll until a stop is requested. Zero the event and set struct_size BEFORE each call (wsrv.h):
 * on WSRV_TIMEOUT the library leaves it untouched, so a stale event must never be released. */
static void run(wsrv_server *server) {
    while (!g_stop_requested) {
        wsrv_event event;
        memset(&event, 0, sizeof event);
        event.struct_size = (uint32_t)sizeof event;
        const wsrv_status status = wsrv_poll(server, &event, 100);
        if (status == WSRV_TIMEOUT) {
            continue;
        }
        if (status != WSRV_OK) {
            fprintf(stderr, "echo_c: wsrv_poll: %s\n", wsrv_status_name((int)status));
            return;
        }
        if (event.type == WSRV_EV_TEXT || event.type == WSRV_EV_BINARY) {
            echo_message(server, &event);
        }
        /* OPEN, CLOSE and ERROR need no action for an echo. */
        wsrv_event_release(&event);
    }
}

static int install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGINT, &action, NULL) == 0 && sigaction(SIGTERM, &action, NULL) == 0;
}

int main(int argc, char **argv) {
    options opts;
    wsrv_config *config;
    wsrv_server *server = NULL;
    wsrv_error error;
    int ok;

    if (!parse_args(argc, argv, &opts) || !install_signal_handlers()) {
        return EXIT_FAILURE;
    }
    config = wsrv_config_new();
    if (config == NULL) {
        fputs("echo_c: out of memory\n", stderr);
        return EXIT_FAILURE;
    }
    ok = configure(config, &opts);
    if (ok && wsrv_server_create(config, &server, &error) != WSRV_OK) {
        fprintf(stderr, "echo_c: %s\n", error.message);
        ok = 0;
    }
    wsrv_config_free(config); /* the server keeps its own copy */
    if (!ok) {
        return EXIT_FAILURE;
    }
    if (wsrv_server_start(server, &error) != WSRV_OK) {
        fprintf(stderr, "echo_c: %s\n", error.message);
        wsrv_server_destroy(server);
        return EXIT_FAILURE;
    }
    printf("echo_c: listening (%s) on %s:%u (Ctrl-C to stop)\n",
           opts.tls_cert != NULL ? "wss" : "ws",
           opts.host,
           (unsigned)wsrv_server_port(server));
    fflush(stdout); /* flushed: scripts wait for this line */

    run(server);

    /* destroy stops the server (clients get Close 1001), waits for the I/O threads and frees all. */
    wsrv_server_destroy(server);
    return EXIT_SUCCESS;
}
