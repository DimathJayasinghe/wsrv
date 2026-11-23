# ADR-0029: The client address behind a reverse proxy

- Status: **Accepted** (2026-11-23; the owner asked for the production gaps to be closed before the stable release, and this was the first of them)
- Date: 2026-11-23
- Deciders: project owner, maintainer assistant

## Context
Almost every production deployment puts `wsrvd` behind something: nginx, Caddy, a cloud load balancer, a Kubernetes ingress. Then the TCP peer of every connection is the proxy. Three things break:
- **Per-IP limits (ADR-0021)** see one address for all users, so the default 100 connections per IP refuses real users; the documented workaround was to switch the limits off, which removes the defence.
- **The application** (`open` event, the handshake hook) is told the proxy's address as the client's.
- Logs name the proxy.

`docs/OPERATIONS.md` and `PROTOCOL_EDGE_CASES` (EC-N21) said `trusted_proxies`, PROXY protocol and `X-Forwarded-For` were supported. None was implemented: the documents promised what the code did not do. This ADR builds the first, most common part and corrects the text for the rest.

## Options considered
1. **PROXY protocol (v1/v2).** The proxy prepends a small binary or text header to the TCP stream; it is known at accept time, so the per-IP limits work unchanged. Used by AWS NLB, HAProxy, nginx `stream`. But it needs a pre-handshake read stage in the connection, and does not help the most common PHP setup: an HTTP reverse proxy (nginx `proxy_pass`, Caddy, ingress-nginx, ALB, Cloudflare) that forwards the client in a header.
2. **A forwarded header (`X-Forwarded-For`, or a single-valued one such as `X-Real-IP`, `CF-Connecting-IP`)** read from the upgrade request, trusted only from configured proxies (this ADR).
3. **Trust the header from anyone.** Rejected: a client could then claim any address, and with it any other user's per-IP budget or the address the application logs. The whole value of the feature is that it cannot be spoofed.

## Decision (header first; PROXY protocol is a later, separate decision)

### 1. Configuration
- `trusted_proxy`: a repeated key, each value an address or a CIDR (`10.0.0.0/8`, `192.168.1.5`, `::1`, `fd00::/8`). At most 64 entries. **Empty (the default) switches the feature off**: no header is ever read.
- `client_ip_header`: the header name, default `X-Forwarded-For`. That one is read as a **list** (RFC 7239 style, comma separated, several header lines joined in order); any other name is read as a **single value** (`X-Real-IP`, `CF-Connecting-IP`, `True-Client-IP`).
- Both are keys of `ServerConfig`, of the embedded text configuration and so of `wsrvd`.

### 2. Who may be believed
The header is read **only if the TCP peer is inside `trusted_proxy`**. From any other peer it is ignored without a log line and without error: a client that sends `X-Forwarded-For` straight to the daemon gains nothing.

### 3. Which entry is the client (list header)
Walk the list **from the right**. The rightmost entry was appended by the proxy that talked to us. While an entry is itself a trusted proxy, move one step left; the first entry that is not trusted is the client. Everything left of it is controlled by whoever sits there, so it is never used. If **every** entry is trusted (an internal caller going through the same proxy), the leftmost is the client. This is the usual "rightmost untrusted" rule.
For a single-valued header the one value is the client; two header lines of the same name are not accepted.

### 4. Fail safe
Anything unusable falls back to the **proxy's own address**, so the limits get stricter, never weaker: no header, an entry that is not an address, more than 32 entries, a value over 1 KiB, an empty list. An entry may be `1.2.3.4`, `1.2.3.4:5678`, `2001:db8::1`, `[2001:db8::1]` or `[2001:db8::1]:443`; no zone ids, no names.

### 5. What changes for the limits (ADR-0021)
- A connection from a trusted proxy takes **no per-IP slot at accept** (the proxy is the operator's own and would otherwise exhaust its own budget), but still counts against the global `max_connections`.
- When the upgrade request is valid, the client address is resolved and **admitted through the same `IpLimits` at that moment**; over the limit the answer is `429` (or `503` if the table is full) and the connection ends, like any refusal of the handshake. The slot is released under the address it was admitted under.
- The address handed to the application (`ConnectionInfo.peer`, `Session::peer()`, the handshake hook, the `Open` event) is the resolved client, with port 0 (the port of a client behind a proxy is not known). Without a trusted proxy nothing changes.
- The new-connection rate limit now counts the handshakes of that client address, not the accepts of the proxy.

### 6. What is not here
PROXY protocol v1/v2 (a later decision; its place in the connection is before the handshake), `Forwarded` (RFC 7239) parsing, and trusting a header on the admin or publish listeners (they do not read client addresses).

## Consequences
+ Per-IP limits and the application's view of the client are right behind nginx, Caddy, ALB, Cloudflare and ingress controllers, with the limits left on.
+ Nothing is trusted by default, and a spoofed header from an untrusted peer has no effect.
− A connection from a trusted proxy is limited per client only after its upgrade request arrived, not at accept: the proxy can have many handshakes in flight (bounded by the global cap and the handshake timeout).
− A misconfigured proxy that forgets the header makes all its users share the proxy's budget: that shows up as `429`s, which is the safe direction.
− PROXY-protocol-only environments (TCP load balancers without HTTP awareness) still need the limits raised or switched off until the follow-up.
