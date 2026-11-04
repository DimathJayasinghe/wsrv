# Lesson 25 — Turning TLS on from configuration: failing closed and reloading on SIGHUP

## Goal
After this lesson you can explain why "TLS requested but not available" must stop the server instead of falling back, how a certificate is replaced without dropping anyone, and how a Unix daemon is told to re-read files.

## 1. Fail closed
An operator who writes `tls_cert=...` expects `wss://`. If anything stops TLS from working (missing file, expired certificate, a build without OpenSSL) and the server quietly serves plain `ws://` instead, passwords and cookies cross the network in the clear and nobody notices until it is too late. So `start()` returns an error for all of these, and the daemon exits with the reason. **Rule: when a security feature is asked for and fails, the system stops; it does not degrade silently.**

The same reasoning is why *one* of the two keys already counts as "TLS requested": `tls_cert` without `tls_key` is a typo to be reported, not a request for plain text.

## 2. Where the check lives
`start()` loads the certificate on the caller's thread **before** spawning the I/O thread. Compare with the alternative (load on the I/O thread): the error would have to be passed back through the promise and there would be a moment where the thread exists but cannot serve. Doing the work first means the failure path owns nothing (no thread to join, `started_` reset so the host may fix the settings and retry).

## 3. Replacing a certificate without dropping anyone
Lesson 22: `TlsServerContext::reload()` builds a complete new context, validates it, and only then swaps it in. Each new connection takes the *current* context once, in `TlsTransportWrapper::wrap`. A connection that already has its context keeps it (OpenSSL reference-counts it). Two consequences, both tested: a connection opened before the reload still works after it; a connection opened after sees the new certificate (the test compares the certificate's common name).

A failed reload changes nothing. The test breaks the file on purpose and checks that the *old* certificate is still served.

## 4. SIGHUP
Unix convention: "hang up" originally meant the terminal closed; daemons reuse it as "re-read your configuration". `wsrvd` already reads signals from a descriptor (the `SignalWatcher`, lesson 11) instead of using asynchronous handlers, so the reload runs as ordinary code on the daemon's loop thread, between two events. That is why it may allocate, read files and log, which a classic signal handler may not.

One detail: the handler needs the server, which is created *after* the watcher. A pointer set when the server exists and cleared before it is closed bridges that gap; since both the handler and the setter run on the same thread no lock is needed.

## 5. Reporting why
`TlsServerContext` writes the file name and OpenSSL's reason to a logger and returns a fixed-text `Error` (errors never carry OpenSSL's text; lesson 22). The embedded server keeps the **first** error line since the reload began: the context logs the specific line (`cannot use the certificate chain '/x/cert.pem': PEM lib`) and then a generic summary (`reload failed, keeping the certificate in use`). Our first version kept the last line and a test that asked "does the message name the file?" failed. Tests that check the *content* of an error message catch this; tests that only check "an error happened" do not.

## Pitfalls
- A header that includes `tls/...` drags the TLS module into everything that includes it. A nested type defined in the `.cpp` keeps the header the same for both builds.
- Do not test only the TLS build: configure once with `-DWSRV_ENABLE_TLS=OFF` and run the "asking for TLS must fail" test there.
- A new C function is an ABI change: header, `wsrv.symbols`, version bump, and a consumer check, all in the same commit (the symbol tests fail otherwise).

## Exercises
1. Make `reload_tls()` run while a connection is being accepted on another loop. Which lock protects the swap, and for how long is it held?
2. Add a metric stub: increment a counter in `reload_tls_on_hangup` for success and failure. What should the label values be?
3. Why does `wsrvd` clear its `reloadable` pointer *before* destroying the server rather than after?
