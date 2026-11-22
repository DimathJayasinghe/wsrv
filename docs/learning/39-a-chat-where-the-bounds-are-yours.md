# Lesson 39 — A chat where the bounds are yours (PHP chat on the embedded server)

## Goal
After this lesson you can say which limits a library gives you and which an application must add, and you can point at the line where each one is enforced in `bindings/php/examples/chat/Chat.php`.

## The problem
The C++ chat (lesson 34) is thin because the broker enforces the bounds: room name rules, rooms per connection, payload size. PHP cannot use the broker yet (exposing it needs its own C ABI design). So the PHP chat keeps its rooms in two arrays, and nothing stops a client from joining a million rooms with a thousand-character name... unless the example does.

## 1. Every peer-sized thing gets a constant
`Chat` has four: `MAX_ROOM_NAME` (64 bytes of `[A-Za-z0-9._-]`), `MAX_ROOMS_PER_CONNECTION` (16), `MAX_ROOMS` (1024 on the whole server), `MAX_TEXT` (4096). That is CLAUDE.md's rule ("no allocation sized from peer data without a configured bound") applied to PHP arrays: the rooms table can never hold more than 1024 names, each at most 64 bytes, and each connection appears in at most 16 of them. The tests hit each edge (65-character name, 17th room, 4097-byte text).

The global cap needs its own sentence: sixteen connections that each join 64 distinct rooms (if the per-connection limit allowed it) would fill the table, so the per-connection bound and the global bound are two different protections. An application that serves strangers should add a per-account quota before this.

## 2. What the library already did
- **Valid UTF-8.** A text message that reaches `onMessage` is valid UTF-8; the library closed the connection with 1007 otherwise. The first draft of this example had its own check, and the test for it never got a reply: the connection was already closed. Dead code that looks like protection is worse than none, so it is gone and a comment says why.
- **Message size and frame rate.** `max_message_size` and `max_data_frames_per_second` bound what arrives at all.
- **Closing.** Every connection ends with exactly one `close` event, so `onClose` giving the rooms back cannot be skipped or doubled. The test disconnects a client without a Close frame and waits for the room count to fall to zero.

## 3. Policy, not safety
You may only speak in a room you joined, and only ask `members` about one you joined. The second matters more than it looks: an open `members` lets anyone probe which rooms exist. The sender is left out of its own broadcast, and the reply to `say` carries `accepted` and `refused` counts, because `send()` returns `false` for a full queue: the sender can see load being shed instead of a silent loss.

## Tests
`chat_interop.php` (CTest `php_chat_interop`, release builds): the real library, two raw clients, one PHP thread. Join, speak, members, rooms; a stranger cannot speak or probe; every bound above; binary is refused; leaving; a client that vanishes gives its rooms back.

## Exercises
1. Add `nick <name>`. Which constant bounds its length, and what would you add for its characters?
2. Make `MAX_ROOMS_PER_CONNECTION` 1000 and keep `MAX_ROOMS` at 1024. Write the three-client script that fills the table. What does the fourth client see?
3. A member's queue is full for a minute. What does `say` report, and how would you make the chat close such a client instead (hint: the echo example's 1013)?
