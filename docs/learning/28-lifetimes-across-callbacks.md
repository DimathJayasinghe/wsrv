# Lesson 28: Lifetimes across callbacks (a use-after-free found by review)

**Milestone:** M9 (security review of the permessage-deflate core) · **Concepts:** views and ownership, re-entrancy, "release at a safe point", failing closed, not trusting injected components

## Goal

After this lesson you can explain how a perfectly tidy-looking `close()` call could free memory that the caller was still reading, why the bug was invisible in small tests, and the three habits that catch this class of bug: tests that make the memory really go away, mutation checks, and validating what an injected collaborator tells you.

## Background: the problem

`on_text(Session&, std::string_view text)` hands the application a **view**: a pointer and a length into the assembler's buffer. A view owns nothing; it is valid only as long as the owner keeps the bytes. The documented contract was: "the view stays valid until the next `handle()` call".

Now the application does something perfectly reasonable inside the callback:

```cpp
void on_text(Session& s, std::string_view text) override {
    s.close(CloseCode::Normal, "enough");   // 1. we are finished with this client
    log(text);                              // 2. ...but we still read what it sent
}
```

`close()` makes the session queue a Close frame and **discard the half-delivered message**. The discard released the buffer immediately. Our `ByteBuffer` really frees its storage when it holds more than 16 KiB, so line 2 read freed memory: a heap-use-after-free. The server itself could trigger the same thing: with the default slow-consumer policy, a `send_text()` that hits backpressure closes the connection from inside the call, again while the handler holds its view.

Why did nobody see it? The existing close-in-callback test used a tiny message and never touched `text` after `close()`. Below 16 KiB the buffer keeps its storage, so the stale read still returned the right bytes. **A use-after-free that "works" is still a use-after-free.**

## Design: release at a safe point

Two rules the fix follows:

1. **The owner decides when memory goes away, and it must be a moment when no borrowed view can exist.** "Right now, inside someone else's callback" is never such a moment. The next `handle()` call is: by contract the previous view is dead by then.
2. **Reuse the mechanism that already exists.** A normally delivered message already sets `clear_buffer_ = true` and the next `handle()` clears. `discard_message()` now does the same, instead of clearing at once.

One trap: the code path that starts a *fresh* message used to call `discard_message()` too. If that call now set the flag, the next event of the new message would wipe its own bytes. So the "reset the counters" part was split out (`reset_message_state()`), and only the discard sets the flag. This is the kind of detail a test must pin: `ADeliveredViewSurvivesDiscardMessageUntilTheNextHandle` also checks that the *next* message still arrives intact.

## Walkthrough

```cpp
// message_assembler.cpp
void MessageAssembler::discard_message() noexcept {
    reset_message_state();      // counters, UTF-8 state, "draining" flag
    clear_buffer_ = true;       // storage is released by the next handle(), not now
}
```

`reset()` (used when the assembler is reused wholesale, never from inside a delivery) still clears immediately.

## Tests: what proves it

- `AMessageViewStaysValidAfterTheHandlerClosesInsideTheCallback`: a 20 KiB message (above the 16 KiB retained capacity), the handler closes and then **copies** the view. Under ASan the old code aborts with `heap-use-after-free ... READ of size 20480`.
- The same with the slow-consumer policy closing inside `send_text`.
- An assembler-level unit test of the contract, so the rule is pinned where it lives, not only through the server.
- **Mutation check:** after the fix we temporarily put the immediate `buffer_.clear()` back and confirmed that both connection tests fail under ASan. A test you have never seen fail proves nothing.

## The same review, four more lessons in small

| Finding | The mistake | The rule |
|---|---|---|
| `allow_rsv1` could be set by the caller | A flag combined with `\|\|`: a second way to turn the feature "on" with nothing behind it | Derive safety flags from the single source of truth; never OR them with caller input. Fail closed. |
| Ratio limit refused an honest 4:1 message | Counted bytes zlib had *consumed*, which depends on content order | A limit must be measured on what the peer *sent*, the thing it controls and the doc promised |
| Injected inflater returned `consumed > input` or no progress | The session indexed spans with numbers it did not check; a no-progress step looped for ever | Anything behind an interface is untrusted input too: validate its numbers, fail with 1011 |
| Compression scratch kept 64 KiB per idle connection | "Retain a buffer for speed" multiplied by 10 000 connections | Per-connection memory is a budget: release it unless measured otherwise |

And one build-level habit: release builds now carry `-D_GLIBCXX_ASSERTIONS` (libc++: the hardening mode), so an out-of-range `std::span` index aborts instead of being silent undefined behaviour. `tests/tools/check_release_flags.py` fails if any translation unit lacks it.

## Pitfalls

- ASan only reports memory that was actually freed. Make the test **big enough** that the allocator really releases (here: above the retained capacity), otherwise it passes by luck.
- Do not "fix" a lifetime bug by copying the data in the library. That hides the contract, costs on every message, and the next view you hand out has the same problem. Fix the owner's release timing.
- A defensive `assert` for an invariant you have only *read* to be true can become a remote crash if you are wrong. We left one out on purpose (see the review notes in ADR-0020 A1) and kept the test-side check instead.

## Exercises

1. Hand out a `Part` view in streaming mode and call `close()` inside `on_message_part`. Which buffer does the view point into, and when is it overwritten? Write the ASan test.
2. `ByteBuffer::clear()` keeps up to 16 KiB. Change the constant to 1 KiB in a scratch build; which existing tests change behaviour, and why does that make small messages a poor test of lifetimes?
3. Add a fake inflater that returns `produced == out.size()` forever. Does the session terminate? What bounds it?
