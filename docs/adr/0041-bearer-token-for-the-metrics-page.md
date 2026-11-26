# ADR-0041: An optional bearer token for the admin listener's `/metrics` page

- Status: **Proposed** (2026-11-26; the owner chose the scope in chat: optional, not required; the owner accepts the ADR). Amends ADR-0023 section 5 ("authentication of the admin port ... loopback or network policy is the control"). Closes the "admin listener has no auth" point of the second independent review.
- Date: 2026-11-26
- Deciders: project owner, maintainer assistant

## Context
The admin listener (ADR-0023) serves three pages over plain HTTP: `/metrics`, `/healthz`, `/readyz`. It is off unless `admin_port` is set and binds to `127.0.0.1` unless `admin_host` says otherwise. Nothing authenticates it. The control is the network: loopback, a firewall, a Kubernetes NetworkPolicy. SECURITY_DESIGN T21 records this as known and accepted, including that a page on the same machine can read the counters through DNS rebinding (there is no `Host` check).

That control is thin exactly where the daemon is usually run behind a scraper. The official image binds the admin port to the container's own loopback (it sets `WSRVD_ADMIN_PORT=9101` and leaves the host alone), so a Docker user who wants Prometheus in another container to reach it must set `WSRVD_ADMIN_HOST=0.0.0.0`; the reference Kubernetes manifest does exactly that. From then on anything that can open a connection to the port reads `/metrics`: connection counts, message and byte rates, how many clients each limit refused, close-code classes. The label set is closed (ADR-0023 section 1, no peer data in any label), so this is aggregate traffic information, not user data. It is still information an operator did not decide to give away, and the review that raised it asked for a credential.

A constraint shapes everything below: **`/healthz` and `/readyz` cannot carry a credential.** The Kubernetes readiness probe is an `httpGet` on `/readyz` (a kubelet cannot read a Secret into a request header), and `wsrvd --healthcheck`, which the image's `HEALTHCHECK` and the liveness probe run (ADR-0030), sends `GET /healthz` with no header. Both pages answer a fixed word and reveal only "up" and "ready".

There is a precedent. The publish endpoint (ADR-0028, ADR-0039) already has a bearer token, a file with strict rules, a constant-time compare and one `401` answer. The difference in stakes is why this ADR makes the token optional and that one hard: with the publish port anyone can inject messages into rooms; with `/metrics` anyone can read aggregate counters.

## Decision

### 1. One key, on the daemon: `admin_token_file`
`wsrvd` gets `admin_token_file` (config file, `--admin-token-file`, `WSRVD_ADMIN_TOKEN_FILE`), the same shape as `publish_token_file`: the path of a file holding the token, never the token itself (no `admin_token` key: a value on a command line shows in `ps` and `/proc`, as for the other secrets). The file follows the rules of `read_token_file` (a regular file, owned by the service user, mode 0600, one trailing newline ignored) and the token must be one `wsrvd --generate-token=FILE` made (the form of ADR-0038 section 3 and ADR-0039: a token somebody chose can be guessed however long it is).

The key needs `admin_port`: a token without an admin listener is a configuration error that says so, as `publish_token_file` without `publish_port` is.

For an application that embeds the server, the credential is a C++ setter on `EmbeddedConfig` (`set_admin_token`) and a borrowed pointer in `ServerDeps`, like the publish token. **It is not a C ABI key or symbol**: the publish token is not one either, and a secret does not belong in a `wsrv_config_set` string. The C ABI stays 1.1 (ADR-0040). The embedded path keeps the length floor of the publish token as the last defence (`kMinAdminTokenBytes` = 32): `set_admin_token` refuses a shorter token, and so does `Server::start_admin()` for a builder used directly; the generated form is enforced by `wsrvd` only, as for the publish token (a library user who chooses a weak token has chosen it).

### 2. What it protects: `/metrics` only
With a token configured, `GET /metrics` needs `Authorization: Bearer <token>`. `GET /healthz` and `GET /readyz` stay open, for the reason in the Context. Without a token nothing changes: every page answers as before, and the existing `admin_exposed` warning stays for a non-loopback bind.

**Optional changes nothing by default.** A deployment that binds the admin port to `0.0.0.0` and does not set the key is exactly as exposed as it is today. The warning remains, and the documentation (OPERATIONS 3.1, the Kubernetes manifest) tells the operator how to close it. Making the token required on a non-loopback bind was considered and not chosen (see Options).

### 3. The answer, and the order of checks
- A missing, wrong, malformed or repeated credential gets **one answer**: `401 Unauthorized` with `WWW-Authenticate: Bearer`, the same body for all, `Connection: close`, as the publish endpoint does. The peer learns nothing about which part was wrong.
- The credential is compared in constant time (`constant_time_equal`, `base/`) **before** `metrics_text()` is called. An unauthenticated peer cannot make loop 0 render the page.
- Routing is unchanged and comes first: the request line is judged exactly as today (method, exact target, version). The credential is only looked at for `GET /metrics`. A wrong method or target still answers `405` or `404`, with or without a credential.

### 4. How the header is read
Today the admin parser never looks at headers. With a token configured, for `GET /metrics` only, the head (at most `kMaxAdminHeadBytes`, 2 KiB) goes through the project's strict head parser (`HttpRequestParser`: CRLF only, no obs-fold, no control characters, token names), and the credential is taken by one shared function. `bearer_of` is today private to `http/publish_request.cpp`; it moves to a public helper in `http/` and both endpoints use it, so there is **one** definition of "exactly one `Authorization`, scheme `Bearer`, one space, a credential with no space" and not two that can drift. A head the strict parser refuses is `400`, as for the publish endpoint.

The no-token path is not touched: the same function, the same bytes, so the existing admin tests are the regression guard for everyone who does not set the key.

### 5. The token must differ from the other two
At start `wsrvd` refuses an admin token equal to the publish token or to the IPC token (when those are set), and says which. The party that holds the admin token is a monitoring system that is not meant to publish into rooms or to speak as the application's worker (over TCP the IPC token is the only authentication, ADR-0038). One shared token would make a Prometheus credential a write credential. A comparison at start is cheap; the failure message names the fix (`wsrvd --generate-token` for a new one).

### 6. What an operator sees
- Start-up: with a token, a non-loopback bind logs `admin_listening` at info and **not** the `admin_exposed` warning, whose text ("has no authentication") would be wrong for `/metrics`; the info line says that `/metrics` needs the token and that `/healthz` and `/readyz` do not.
- A rejected `/metrics` request logs a rate-limited `warn` event `admin_unauthorized` (the 1st, 2nd, 4th, 8th, ... one, like `ip_limit` and `buffer_budget`). No new metric: the page that would show it is the one that was refused.

### 7. What this does not do
- **No TLS.** The token travels in clear on a non-loopback bind. It keeps out whoever can *reach* the port but not read the traffic; against a network eavesdropper only a private network or TLS in front helps. OPERATIONS says so.
- **No lockout and no rate limit on wrong guesses.** A generated token has 256 bits; guessing is not a threat, and a lockout hands an attacker a way to lock the monitoring out (the lesson of ADR-0038 section 5).
- **The eight-slot exhaustion of T21 is unchanged.** The request head is read before the credential is judged, so eight slow clients can still hold every slot for 5 seconds at a time and make `/healthz` unanswerable. The token does not touch it; network placement does.
- **DNS rebinding:** with a token, a page cannot read `/metrics` (it does not know the token). `/healthz` and `/readyz` remain readable by such a page; they hold one word each.
- **Rotation is a restart.** SIGHUP reloads the TLS certificate only (`wsrvd/reload.hpp`); the token file is read once at start, like the other two.

## Options considered
1. **Do nothing; keep "the network is the control".** Honest and simple; leaves the default Docker and Kubernetes setups with a readable `/metrics`, and the review's point unanswered.
2. **A token on all three pages.** Breaks the Kubernetes readiness probe, the container `HEALTHCHECK` and the liveness probe, none of which can carry a credential, and protects two pages that reveal one word each.
3. **The token required whenever `admin_host` is not loopback** (like the publish token, no escape hatch). Closes the gap by default. Not chosen by the owner: `/metrics` holds aggregate counters only, unlike publish, and the cost is large (a second Secret in the reference manifest, a change to the `kind` job, a credential in every Prometheus config, and a token even for Docker users who publish the port only to the host's loopback, which the daemon cannot see). It can still be added in a later minor by a later ADR; the optional key is the building block.
4. **An allow-list of client addresses** (like `trusted_proxy`). Pod and scraper addresses change, and a NetworkPolicy does this job where the platform supports it.
5. **TLS or mutual TLS on the admin listener.** The right answer for a hostile network, and a feature of its own (certificates for a second listener); a later ADR.
6. **An optional token on `/metrics` only (this ADR).**

## Consequences
+ An operator who exposes the admin port can require a credential for the one page that matters, without breaking probes. One shared `bearer_of` for two endpoints removes a second place for a header-parsing mistake.
+ Nothing changes for anyone who does not set the key.
- The default exposure of a `0.0.0.0` admin port stays until the operator sets the key (see section 2).
- The token is plaintext on the wire; so is every other admin page.
- Prometheus (or any scraper) must be given the token: `authorization: {credentials_file: ...}` in a scrape config. OPERATIONS 3.1 shows it.

## Slices
1. This ADR; `bearer_of` moved to a public `http/` helper, with the publish tests as the regression guard and one mutation (accept a repeated `Authorization`) run against them.
2. The admin check: `AdminSources` gets the credential, `admin_response` applies section 3 and 4, `Server::start_admin` and `ServerDeps` carry it, `EmbeddedConfig::set_admin_token`, with tests first (401 for no, wrong, repeated and malformed credentials; 200 with the right one; `/healthz` and `/readyz` 200 with a token set and no header; 404 and 405 unchanged).
3. The daemon: `admin_token_file`, `read_secrets` (generated form, different from the publish and IPC tokens), usage text, start-up tests.
4. Documentation: OPERATIONS 3.1 (including the Prometheus `authorization` block), SECURITY_DESIGN T21, the reference manifest (optional, commented), CHANGELOG, lesson 54.
