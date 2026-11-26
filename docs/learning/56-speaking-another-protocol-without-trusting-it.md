# Lesson 56 — Speaking another protocol without trusting it

## Goal
After this lesson you can explain why the Pusher protocol (what Laravel Echo speaks) lives in the PHP worker and not in the C++ daemon, how a signature stops a browser from joining a channel it was not allowed to, why presence needs state and where that state may live, and three habits that found real bugs while this was built: mutation-checking the tests, reading what you assume cannot happen, and refusing to claim what you have not run.

## The problem
Laravel developers write `broadcast(new OrderShipped)` and, in the browser, `Echo.private('orders.7').listen(...)`. Echo speaks the **Pusher WebSocket protocol**: JSON frames such as `{"event":"pusher:subscribe","data":{"channel":"private-orders.7","auth":"key:signature"}}`. wsrvd speaks plain WebSocket and knows rooms, not Pusher. To make Echo work, something must turn those frames into joins and leaves, and something must decide who may join.

## 1. Where does the protocol live? (ADR-0042)
Three options were weighed: in the C++ daemon, in the PHP worker, or in a separate proxy. The daemon keeps its promise of being protocol-agnostic (it never parses JSON, and a bug in a parser there is a bug in the process that holds every socket). So the **gateway lives in the PHP worker**: the daemon already tells the worker about every open, message and close, and the worker can subscribe a connection to a room, publish, and close. Fan-out stays in C++. The price: a PHP process is on the join path, so it must be written as if every frame is hostile, and the daemon's rules (a topic alphabet, bounded rooms) show through, which is why `=`, `,` and `;` in channel names are refused rather than rewritten.

*Pattern:* an **adapter** at the edge, with the core staying small and ignorant. A choice like this is an ADR because a reader in a year will ask "why is there JSON handling in the worker?".

## 2. A signature that cannot be replayed
A private channel must only be joined by users your Laravel code approved. The browser asks `/broadcasting/auth` (your `Broadcast::channel` callback decides) and receives `key:HMAC-SHA256(secret, socket_id:channel)`. The gateway recomputes it when the browser subscribes. What this buys:

- **Bound to the socket id**: the id names *this* connection, so a signature copied to another browser fails.
- **Bound to the channel**: a signature for `private-a` does not open `private-b`.
- **Presence data is inside the signature** (`socket_id:channel:channel_data`): the user id and name the browser sends are exactly the ones your server signed. A browser cannot join as user 1 with user 7's signature.
- **`hash_equals`**, not `===`: a plain comparison can leak, through timing, how many leading characters were right. A test cannot measure timing reliably, so one test pins the source instead (a rare, honest use of "assert the code contains X").
- The secret is never in a message, a log, a stack trace (`#[\SensitiveParameter]`), `print_r`, `var_dump` or `serialize`; tests check each.

## 3. Presence is state
"Who is here?" needs a table: channel → users → connections. Two rules fall out: a *user* with two tabs is one member until the last tab leaves, and the table must never outlive the connections it describes. The worker holds it in memory; when the worker dies, the daemon closes every browser, so there is nothing stale to repair. That only holds for **one daemon**, which is why the first release says so instead of pretending.

The daemon answers a subscribe *later* (a STATUS with the request id). So the gateway keeps a map "request id → what this join was for". The interesting bug-shaped case is two users racing for the last seat: both looked free when asked, both get OK, the second is refused when its answer arrives and leaves the room again. There is a test for exactly that.

## 4. Habits that found real problems
- **A value that decodes but cannot encode.** `{"n":1e999}` is valid JSON; PHP decodes it to infinity; encoding infinity throws. A whisper like that reached `json_encode` and raised an exception out of the handler. It was found by the automated security review of the commit, fixed by treating it as a bad frame (4200, a strike), and covered by tests for whispers and presence data. Lesson: after "parse", ask "can I still print what I parsed?"
- **Mutation-checking the tests.** After the gateway tests passed, seven deliberate breakages (verify always true, channel_data not signed, the sender not excluded, the pending join kept after close, member cap off, the browser's user id trusted, `hash_equals` replaced by `==`) were applied one at a time. Six made tests fail; the seventh did not, which is how the "source pin" test came to exist. A test that cannot fail is decoration.
- **Reading what already exists.** The Laravel guide already told people to define their own `php artisan wsrv:work`. The new package command with that name would have clashed after an upgrade, so it is `wsrv:echo`. Found by searching the docs before naming.
- **Do not claim what you have not run, and then run it.** Frames, signatures and error codes were first checked against a real server and the SDK, and the glue against a real Laravel application; the guide said in plain words that the real browser libraries had not been run. Then they were (`pusher-js` 8.6.0 and `laravel-echo` 2.6.3 in Chrome, first against a stand-in for the Laravel routes, then against a real Laravel application, where the CSRF header, the session and the `auth` middleware are the real ones). It corrected two of my assumptions without finding a gateway bug: the library posts its auth request form-encoded (my test stub assumed JSON), and for a join followed at once by a leave it still sends `pusher:subscribe` and only unsubscribes after the success, so the other members see a join then a leave, which is also what Pusher does. A test that encodes a wrong belief about the other side fails in a useful way: read the other side's source and fix the belief, not the server.

## Pitfalls
- Treating the public app key as a secret, or the secret as a key: the key is in your JavaScript; the secret signs.
- Sending a plaintext event to a channel the browser expects to be encrypted: the broadcaster refuses instead.
- Forgetting that `toOthers()` leaves a *connection number* out, which is only unique within one daemon.
- Letting a browser choose fields the server should own: a whisper's user id and extra fields are built by the server.

## Exercises
1. Two browsers share one signature for `private-chat.1` (one copied it from the other's network tab). Which browser succeeds, and which line of the gateway rejects the other?
2. A presence user opens three tabs and closes two. How many `member_removed` events are sent? Which test shows it?
3. Change `max_presence_members` to 1 and join with two different users at once. Trace which answer the second user gets and why it is sent after the daemon's OK.
4. Write the test that would catch `JSON_THROW_ON_ERROR` being removed from `Frame::encode` for a hostile whisper.
5. The gateway answers `pusher:signin` with 4200. Why is that safer than ignoring it, and what does a legitimate Echo user lose?
