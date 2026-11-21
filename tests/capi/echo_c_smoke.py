#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Smoke test of examples/echo_c, the pure-C echo server (M11: a C host can serve a real client).

It starts the real binary, speaks RFC 6455 to it over a socket and checks what a conformance run
depends on, without needing the Autobahn image:
  - handshake: 101 with the right Sec-WebSocket-Accept;
  - a text and a binary message come back unchanged (the host's "send back what came in");
  - invalid UTF-8 is closed with 1007 by the library, the C host never sees it;
  - with --autobahn a 2 MiB frame is echoed (max_frame_payload, max_message_size and
    event_queue_bytes were raised through the C configuration keys of ADR-0016 A1);
  - without --autobahn the same frame is refused with 1009 (the library defaults are untouched);
  - SIGINT ends the program with status 0.
Usage: echo_c_smoke.py PATH_TO_echo_c
"""
import base64
import hashlib
import os
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import time

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"  # RFC 6455 section 1.3
DEADLINE_S = 15


RUNNING: list[subprocess.Popen] = []  # every server started, killed on any failure


def fail(message: str) -> None:
    print(f"echo_c_smoke: FAIL: {message}", file=sys.stderr)
    for proc in RUNNING:
        proc.kill()  # a failed check must not leave a server behind for CTest to wait on
    sys.exit(1)


def read_exact(sock: socket.socket, count: int) -> bytes:
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            fail(f"connection closed after {len(data)} of {count} bytes")
        data += chunk
    return data


def client_frame(opcode: int, payload: bytes) -> bytes:
    """One masked, unfragmented client frame (RFC 6455 section 5.2, clients must mask)."""
    head = bytes([0x80 | opcode])
    length = len(payload)
    if length < 126:
        head += bytes([0x80 | length])
    elif length < 65536:
        head += bytes([0x80 | 126]) + struct.pack(">H", length)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", length)
    mask = os.urandom(4)
    masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
    return head + mask + masked


def server_frame(sock: socket.socket) -> tuple[bool, int, bytes]:
    first, second = read_exact(sock, 2)
    if second & 0x80:
        fail("the server sent a masked frame")
    length = second & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(sock, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(sock, 8))[0]
    return bool(first & 0x80), first & 0x0F, read_exact(sock, length)


def server_message(sock: socket.socket) -> tuple[int, bytes]:
    """One message, whole: the server may split a big echo into several frames (section 5.4)."""
    fin, opcode, payload = server_frame(sock)
    while not fin:
        fin, continuation, more = server_frame(sock)
        if continuation != 0:
            fail(f"expected a continuation frame, got opcode {continuation}")
        payload += more
    return opcode, payload


def connect(port: int) -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", port), timeout=DEADLINE_S)
    key = base64.b64encode(os.urandom(16))
    sock.sendall(
        b"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        b"Sec-WebSocket-Key: " + key + b"\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    head = b""
    while not head.endswith(b"\r\n\r\n"):
        head += read_exact(sock, 1)
    if not head.startswith(b"HTTP/1.1 101"):
        fail(f"no 101: {head[:60]!r}")
    expected = base64.b64encode(hashlib.sha1(key + GUID).digest())
    if b"Sec-WebSocket-Accept: " + expected not in head:
        fail("wrong Sec-WebSocket-Accept")
    return sock


class EchoC:
    """The echo_c process, started on a kernel-chosen port."""

    def __init__(self, binary: str, *extra: str) -> None:
        self.proc = subprocess.Popen(
            [binary, "--host", "127.0.0.1", "--port", "0", "--allow-missing-origin", *extra],
            stdout=subprocess.PIPE,
            text=True,
        )
        RUNNING.append(self.proc)
        self.port = self._wait_for_port()

    def _wait_for_port(self) -> int:
        assert self.proc.stdout is not None
        ready, _, _ = select.select([self.proc.stdout], [], [], DEADLINE_S)
        if not ready:
            self.proc.kill()
            fail("echo_c printed no ready line")
        line = self.proc.stdout.readline()
        match = re.search(r":(\d+) ", line)
        if not match:
            self.proc.kill()
            fail(f"unexpected first line: {line!r}")
        return int(match.group(1))

    def stop(self) -> None:
        self.proc.send_signal(signal.SIGINT)
        try:
            status = self.proc.wait(timeout=DEADLINE_S)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            fail("echo_c did not exit after SIGINT")
        if status != 0:
            fail(f"echo_c exited with status {status} after SIGINT")


def expect_echo(sock: socket.socket, opcode: int, payload: bytes) -> None:
    sock.sendall(client_frame(opcode, payload))
    got_opcode, got = server_message(sock)
    if got_opcode != opcode or got != payload:
        fail(f"opcode {opcode}: echo differs (opcode {got_opcode}, {len(got)} of {len(payload)} bytes)")


def expect_close(sock: socket.socket, code: int) -> None:
    _, opcode, payload = server_frame(sock)
    if opcode != 8 or len(payload) < 2 or struct.unpack(">H", payload[:2])[0] != code:
        fail(f"expected Close {code}, got opcode {opcode} payload {payload[:8]!r}")


def main(binary: str) -> None:
    started = time.monotonic()

    server = EchoC(binary, "--autobahn")
    sock = connect(server.port)
    expect_echo(sock, 1, "hello, wörld".encode())
    expect_echo(sock, 2, os.urandom(2 << 20))  # one 2 MiB frame: above the default 1 MiB limit
    sock.sendall(client_frame(1, b"\xff\xfe"))  # not UTF-8
    expect_close(sock, 1007)
    sock.close()
    server.stop()

    server = EchoC(binary)  # library defaults
    sock = connect(server.port)
    expect_echo(sock, 1, b"still works")
    sock.sendall(client_frame(2, bytes(2 << 20)))
    expect_close(sock, 1009)
    sock.close()
    server.stop()

    print(f"echo_c_smoke: ok ({time.monotonic() - started:.1f} s)")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        fail("usage: echo_c_smoke.py PATH_TO_echo_c")
    main(sys.argv[1])
