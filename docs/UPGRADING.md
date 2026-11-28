# Upgrading

What to do when you move from one release to the next. The full list of changes is in [CHANGELOG.md](../CHANGELOG.md); this page is only what can break or needs a decision.

## From 1.0 to 1.1

**Do this before you start the new daemon:**

1. **Publish token.** If your deployment uses the publish endpoint (`publish_port`, the HTTP endpoint behind `Publisher`) and its token file was made by hand, with `openssl rand`, `head -c 32 /dev/urandom` or anything other than the wsrvd generator, **the 1.1 daemon refuses to start** (ADR-0039). The message names the fix. Make a new token and give the new file to both the daemon and the publishers:

   ```
   wsrvd --generate-token=/run/wsrv/publish.token
   ```

   A token a person chose can be guessed; the generated form (`wsrv1_`, 64 hex digits, a checksum) cannot be, and the endpoint is reachable from the network. There is no switch to turn the rule off. The Unix-socket IPC token is not affected.

2. **Image tag.** `wsrvd` images and the `wsrv/client` package are versioned together; use the same `1.1.x` for both. The IPC protocol is unchanged (0.3) and the C ABI is 1.1 (additive), so a 1.0 worker still talks to a 1.1 daemon and the other way round, but the guides assume both are 1.1.

3. **Message size and the outbound queue (ADR-0043).** A message larger than `2 x max_message_size - 16` bytes can never be sent to one browser. Before 1.1 such a send was accepted and silently dropped; now `wsrv_send_*`, an IPC `SEND_*` and a publish are refused (`INVALID_ARGUMENT`, HTTP 413), and `wsrvd` lowers `ipc_max_frame` to that size plus 14 bytes (it logs `ipc_max_frame_lowered` if you had set a larger one; the daemon still starts). If your worker sends messages near `max_message_size`, check that it handles the error. With **no** `max_message_size` set, the per-connection outbound queue is now 2 MiB, not 1 MiB (the same as `--max-message-size=1048576` always gave), so a message of 1 MiB - 15 bytes or more can be echoed; if you size memory by connections times queue, add 1 MiB per connection (the `max_buffered_bytes` budget bounds the sum).

**What is new and off by default (nothing to do unless you want it):**

| New | Setting | Read |
|---|---|---|
| IPC over TCP, for a worker on macOS, Windows or another machine | `ipc_port`, `ipc_host` (exactly one of `ipc_socket` and `ipc_port`); needs a generated token | [OPERATIONS.md](OPERATIONS.md) section 7, ADR-0038 |
| A global budget for buffered bytes | `max_buffered_bytes` (default 1 GiB) | OPERATIONS section 1.2.3, ADR-0040 |
| A token for `/metrics` | `admin_token_file` | OPERATIONS section 3.1, ADR-0041 |
| Laravel Echo support | `php artisan wsrv:echo` and the `wsrv` broadcasting driver | [LARAVEL_ECHO.md](LARAVEL_ECHO.md), ADR-0042 |

**One default that changed without a setting:** the official image no longer sets `WSRVD_IPC_SOCKET`; the built-in default path is the same (`/run/wsrv/ipc.sock`) and applies when neither `ipc_socket` nor `ipc_port` is set, so every existing way of running the image behaves as before.

**If you adopt Laravel Echo, read this first:** public channels need no signature, so the gateway limits them (`max_public_channels`, default 512), a quarter of that budget is kept for signed-in users (`public_channels_reserved`), and a site that knows its public channel names should list them (`public_channels`); unset means any name. The daemon's `broker_max_topics` (1 024 by default) is global and must be sized for a channel per user. Both are in section 2 of the Echo guide, with the numbers.

## Rolling back

A 1.1 worker and a 1.0 daemon, and the reverse, work with each other, so the daemon and the workers can be upgraded one at a time in either order. Going back to 1.0 after a 1.1 deployment needs only the 1.0 image; a publish token made by the generator is valid for 1.0 too.
