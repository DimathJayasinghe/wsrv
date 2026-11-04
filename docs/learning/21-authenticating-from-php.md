# Lesson 21 — Authenticating a WebSocket connection from PHP

## Goal
After this lesson you can explain why authentication happens *during* the WebSocket handshake and not after it, how a PHP callback decides, what "fail closed" means for a callback API (what each way of getting it wrong turns into), and why the callback returns a value instead of calling `accept()`.

## 1. Where the cookie is
A browser cannot add custom headers to `new WebSocket(url)`, but it *does* send the site's cookies with the upgrade request. That is the one place the session is visible, so that is where the decision belongs (lessons 18 to 20 built the plumbing). After the 101 the connection exists and costs memory; an anonymous visitor who got that far is already a problem.

## 2. The API in one picture
```php
$worker->on('authenticate', function (Upgrade $u) {
    return $u->request->cookie('session') === 'good';   // true = let in, false = 403
});
```
`Upgrade` carries the request (`path`, `query`, `origin`, `offered` subprotocols, forwarded `headers`, `cookie($name)`) and two methods, `accept()` and `reject($status)`. The handler can *return* its decision or call those methods itself, and it can do the second later (return `null`) if the answer comes from somewhere slow.

Why a return value as well? Because the safest default for a callback is the one where **doing nothing and doing the wrong thing both refuse**:

| What the handler does | What the browser gets |
|---|---|
| returns `true` / `false` / `401` / `'chat'` | accepted / 403 / 401 / accepted with that subprotocol |
| returns `null` and never answers | 503 from the daemon's timeout |
| throws | 500 (the worker refuses first, then lets the exception surface) |
| returns garbage (`302`, a name the client did not offer, an array) | 500 and an `UnexpectedValueException` |
| there is no handler at all | 503 |
| the worker process dies | 503 at once (the daemon closes its intake, lesson 20) |

There is no row where a mistake accepts. A test per row checks it, and mutation checks (flip the default to accept, drop the refusal on exceptions) are each caught.

## 3. Parsing the blob safely (again)
`HandshakeRequest::parse` reads the same length-prefixed blob as the C layer (lesson 19), in PHP. The rules are the same: a length is a claim, not a fact, so every read first checks that the bytes exist; a truncated, over-long or padded blob is an error, never a guess. The test cuts a valid blob at every possible length.

Cookies: `cookie('session')` splits `Cookie` headers on `;` (RFC 6265 section 5.4), trims, and matches the **whole** name (`sess` does not find `session`). The value is returned as sent: no URL-decoding, quotes kept. Decoding is the application's decision, because decoding twice is a classic way to get a different value than the one that was checked.

## 4. Pitfalls we hit
- **A blocking test client starves the worker.** The first interop test used a blocking WebSocket connect while the PHP worker was supposed to answer: the worker never ran, the daemon timed out and answered 503. In a single-threaded test the client must *send*, the worker must *step*, and only then the client *reads*. The helpers `ws_request_async` and `ws_finish` encode that order.
- **PHP arrow functions capture by value.** `fn () => $held !== null` copies `$held` when it is created, so a closure that waits for it to change never sees the change. A condition that must observe a variable changed by a handler needs `function () use (&$held)`.
- **Which statuses?** The daemon accepts 400, 401, 403, 404, 429, 500 and 503 and nothing else: a 302 would be an open redirect, and a free-text reason would be a header-injection and information-leak channel. The PHP encoder refuses the rest before a byte is sent, so a mistake is an exception in your code, not a silent timeout.

## 5. Exercises
1. Add `$u->request->origin` to the decision (refuse a second allowed origin for one user). Where is the Origin already checked, and why check it again here?
2. Write the handler that accepts a subprotocol only if the user is an admin (`'admin-v1'`). What happens if the client did not offer it?
3. The handler needs a database. Why is returning `null` and answering later *not* a licence to take ten seconds? What is the browser told after 3 s?
4. Why does `Upgrade` throw a `LogicException` on a second answer instead of ignoring it?
