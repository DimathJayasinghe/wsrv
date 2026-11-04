# Lesson 22 — A hardened TLS context (and how to test TLS without downloading anything)

## Goal
After this lesson you can explain what TLS adds to WebSocket and why we do not write it ourselves, what each line of the hardened profile defends against, why a server must refuse an expired certificate *at load*, how a certificate is replaced without dropping connections, and how the tests make their own certificates.

## 1. Why `wss://`
`ws://` is plain TCP: anyone on the path reads and edits it, and a browser refuses `ws://` from an `https://` page. `wss://` is WebSocket **inside TLS**: the same bytes, but first a handshake in which the server proves its identity with a certificate and both sides agree on keys. We use OpenSSL because TLS is exactly the kind of code you do not write twice (ADR-0005: dependencies only when security-critical and not sensible to write). What we *do* own is the integration: how it is configured (this lesson), and how it plugs into the connection (next slice).

## 2. The profile, line by line
`TlsServerContext` builds one OpenSSL `SSL_CTX` (a "context": settings shared by all connections). Every setting is a defence:

| Setting | Defends against |
|---|---|
| minimum TLS 1.2, maximum 1.3 | downgrade to SSLv3 / TLS 1.0 / 1.1 (POODLE, BEAST era flaws) |
| TLS 1.2 suites: ECDHE + AES-GCM / ChaCha20 only | CBC padding attacks (no CBC), no key recovery from a stolen server key later (ECDHE gives *forward secrecy*), no weak ciphers |
| no RSA key exchange | the same: RSA kex lets someone who records traffic and later steals the key read it all |
| `NO_COMPRESSION` | CRIME (compression leaks secrets through size changes) |
| `NO_RENEGOTIATION` | renegotiation DoS and the old injection flaws |
| no tickets, session cache off | stateless tickets need rotating keys to keep forward secrecy; we do not resume yet, so nothing to get wrong |
| security level 2 | RSA below 2048 bits, SHA-1 signatures: refused by OpenSSL itself |
| ALPN: `http/1.1` or nothing | a client that offers only `h2` or `imap` is trying to speak another protocol to our port: refuse (the "ALPACA" cross-protocol class) |
| `SERVER_PREFERENCE` | the server, not the client, picks among the allowed suites |

Notice the order of defence: the AEAD-only suite list already makes TLS 1.1 impossible (those suites do not exist before 1.2). The minimum-version line is still there, because the next person who adds a suite should not silently re-open the old protocols. A test pins the version range and the option bits directly for that reason.

## 3. A certificate we will not serve
`load` refuses, with a fixed error text and a log line naming the *file* (never key bytes):
- a file that cannot be read or is not PEM;
- a key that does not match the certificate (a classic deployment mistake: new certificate, old key);
- a certificate that is expired or not yet valid **now**. Why at load? Because the alternative is a server that starts happily and fails every handshake, and the operator finds out from users. At start-up it is a hard error; at *reload* (below) it keeps the old certificate;
- a key file readable by "others". A private key that anyone on the machine can read has leaked already. Group-readable is allowed (`0640`, the Debian `ssl-cert` convention) because refusing it would make the common packaged setup impossible. Symbolic links are followed (Let's Encrypt's `live/` directory is full of them);
- a passphrase-protected key. OpenSSL would *prompt on the terminal* and block the server, so the context installs a password callback that says "no password": the key is simply unreadable. Operators who need that terminate TLS in front.

## 4. Replacing a certificate while serving
Certificates expire (Let's Encrypt: every 90 days). `reload()` builds a **complete new** context from the files, validates it exactly like the first load, and only then swaps one shared pointer under a mutex held for the length of a pointer copy. Why this is safe:
- a connection takes `current()` once, at accept, and keeps its own reference (`shared_ptr`) — the old context lives until the last such connection ends, so no handshake changes mid-way;
- if the new files are bad, nothing is swapped: the server keeps serving the old certificate and reports the failure. A broken `certbot` run does not take the site down.
We use a plain mutex rather than `std::atomic<std::shared_ptr>` because libc++ 18 (one of our CI compilers) does not provide it yet; the lock is taken once per accepted connection, not per message.

## 5. Testing without downloading a thing
`tests/support/tls_fixtures.hpp` makes certificates with the OpenSSL API inside the test: a key (EC P-256, RSA 2048 or 1024), a self-signed X.509 with chosen validity dates (an expired one is "valid until an hour ago"), and PEM text. They are written to a fresh temporary directory per test. So: no `openssl` command line, no key file committed to the repository (nothing to leak or to expire in CI), and the exact date edge cases are one number each.

To look at what the server *negotiates* without sockets, `handshake_in_memory` joins a client `SSL` and a server `SSL` with a **BIO pair** (two connected in-memory pipes) and alternates `SSL_do_handshake` on each side until both finish or one fails. The client is configurable (`min_version`, `cipher_list`, `alpn`), so each defence has a test that sends a client which *tries* the weak thing.

**Control experiments.** A refusal test can pass for the wrong reason: a client that cannot speak TLS 1.1 at all is refused by any server. So each "refused" test first connects the *same* client to a deliberately permissive server and asserts that it succeeds. Only then does "our server refuses it" prove something.

## 6. Pitfalls we hit
- **GCC warns about designated initialisers** (`-Wmissing-field-initializers`) for a struct member that has no default initialiser, even when you only meant to set one field. Giving every member `= {}` makes `{.alpn = {"h2"}}` legal under `-Werror`.
- **The cached option.** `WSRV_ENABLE_TLS` was already declared (default OFF), so an existing build directory kept OFF after we changed the default; the new target did not exist. `cmake -DWSRV_ENABLE_TLS=ON` once, or delete the cache.
- **A surviving mutant.** Changing the minimum version to TLS 1.0 broke no test, because the cipher list hid it (section 2). Mutation checking found the gap; the pin test closed it.
- **Mismatch is reported twice.** OpenSSL's `SSL_CTX_use_PrivateKey` also checks key and certificate, but with a generic message. We read the key ourselves and call `X509_check_private_key` first, so the operator gets "the private key does not match the certificate".

## 7. Exercises
1. Change `kSecurityLevel` to 1 and load an RSA 1024 chain. What happens, and which test tells you?
2. Why is `SSL_OP_NO_TICKET` *and* `SSL_CTX_set_num_tickets(0)` needed? (Hint: TLS 1.2 and 1.3 issue tickets differently.)
3. Reload takes the mutex only to swap. Why not hold it while building the new context?
4. A server has three hostnames. What would a single certificate need to contain? (Preview of SNI, slice 6.)
5. Extend `handshake_in_memory` to return the negotiated key-exchange group, and assert X25519 is chosen.
