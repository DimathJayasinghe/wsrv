# Lesson 34 — An application on the broker (the chat example, M10 slice 4a)

## Goal
After this lesson you can build a small application on the public API the way the project intends: a handler per connection, one shared object created in `main`, every peer-supplied string treated as hostile, and a test that is also the documentation.

## What the example is
`examples/chat`: rooms over WebSocket. A client sends one text message per command (`join lobby`, `say lobby hello`, `members lobby`, `rooms`, `leave lobby`); the others in the room receive `msg lobby user7: hello`. About 150 lines in `chat.hpp`, 150 more in `main.cpp` for options and startup.

## 1. The composition root owns the shared object
`main` creates the `Broker`, then the `ChatFactory` that refers to it, then the server. Destruction runs the other way, so the broker outlives every handler and the server. That is the dependency-injection rule from CODING_STANDARDS in practice: no global broker, no singleton; the handler gets a reference through the factory. The one awkward point is the circle "the broker reports to the server's metrics, the server needs the factory, the factory needs the broker": `Broker::use_metrics` closes it after `build()`, and `main` detaches it again (`use_metrics(nullptr)`) before the server dies, because the bank belongs to the server.

## 2. A handler is a small state machine, the protocol is a pure function
`parse_command` takes a line and returns a command or nothing. It is tested without a socket (`ChatCommand.*`): exact words, one space between parts, no case folding, no trimming, so there is exactly one spelling of every command. Rejecting `"join  lobby"` (two spaces) is deliberate: a protocol with several spellings has several places to disagree.

## 3. Trust nothing, add nothing
The example adds **no limits of its own**. The room name is checked by the broker (`InvalidArgument`), the number of rooms per connection by the broker (`BufferFull`), the message size by the broker (`InvalidArgument`). The handler only turns the broker's answer into a sentence. Two rules it does add are about *policy*, not safety: you may only speak in a room you joined (so a stranger cannot spam a room), and the sender is left out of its own broadcast (`except = session.id()`).

## 4. Rooms end with the connection
The handler holds a `BrokerMembership`. Whatever ends the connection, the handler is destroyed and the membership gives every room back; `ChatServerTest.EveryoneLeavingLeavesNothingBehind` disconnects six clients that joined two rooms each, without a Close frame, and waits for the topic count to reach zero.

## 5. Backpressure is visible
The reply to `say` is `ok sent <room> <accepted> <refused>`: the `PublishReport` of ADR-0024 as the user sees it. A client that sees a growing refused count knows the server is shedding load, which is better than a silent loss.

## Tests
`ChatCommand.*`, `ChatServerTest.*` (three loops, real sockets, through `chat/chat.hpp`).

## Pitfalls
- Building the broadcast text from peer data without a size bound; here the broker's payload bound catches it.
- Letting a handler outlive the broker (declare the broker first).
- Keeping a `Session&` instead of a `SessionHandle`: only the handle may be kept.
- Reading the reply of a command and the broadcast of another in the wrong order in a test; the tests read each frame in the order the server sends it.

## Exercises
1. Add a `nick <name>` command. Which existing bound covers the name's length, and what would you add for its characters?
2. A client says `say lobby` without text. Where is that rejected, and what does the user see?
3. Make `members` count only connections that joined in the last hour. What state would the broker need, and why is that a bad fit for it?
