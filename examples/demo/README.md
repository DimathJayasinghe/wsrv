# Browser demo for the echo server

A static page (no framework, no CDN) that connects to the `echo` example and shows what a browser
sees: the handshake, every message with its size, and the close code with its meaning.

## Run it

1. Start the echo server and allow the page's origin. In the dev container (`make shell`):

   ```sh
   cmake --preset dev && cmake --build --preset dev -j --target echo
   ./build/dev/examples/echo --host 0.0.0.0 --port 8080 --allow-origin http://localhost:8000
   ```

   Port 8080 is already published to the host by `docker-compose.yml`.

2. Serve the page from a separate static host (the library speaks WebSocket only; it does not serve
   files):

   ```sh
   python3 -m http.server 8000 -d examples/demo
   ```

3. Open <http://localhost:8000> and press **Connect**.

## Things to try

- **Send text / binary**: the echo comes back identical. A binary message over 64 KiB is split into
  several frames by the server and joined again by the browser; the page sees one message.
- **Send exactly 1 MiB**: accepted and echoed (the default limit is 1 MiB).
- **Send 1 MiB + 1 byte**: the server closes with **1009** before reading the payload (FR-F6).
- **Close with code**: pick 1000 or 3000 to 4999; the server echoes the Close and the page shows the
  code.
- Open the page from `file://`: the handshake is refused with 403 and the page shows **1006**. A
  `file://` page sends `Origin: null`, which is never on the allow-list. That is the protection
  against cross-site WebSocket hijacking working as designed (SECURITY_DESIGN T8).

## What a browser cannot show

The WebSocket API hides frames. There is no way to send a Ping, a fragmented message, or invalid
UTF-8, and the page cannot see individual frames or opcodes; the server answers Pings by itself.
Those behaviours are exercised by the integration tests and by the Autobahn suite
(`tests/autobahn/`).
