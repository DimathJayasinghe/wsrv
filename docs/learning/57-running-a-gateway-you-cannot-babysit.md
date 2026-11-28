# Lesson 57 — Running a gateway you cannot babysit

## Goal
After this lesson you can explain why a strike counter has to forgive, and why it must forgive in whole units; why a statistics line is made of numbers only and why "every gauge is back to zero" is the cheapest leak test there is; how one oversize message can take a whole site down when an error is treated as a lost connection, and the pattern that prevents it; and why a cache needs three bounds and a guard that makes a stale answer impossible, not just unlikely.

## The problem
The gateway of lesson 56 passed its tests and still had four ways to fail in production, none of which a test with small inputs and short lives can show: a browser tab that stays open for a week, a room with a hundred members, a client that repeats a question as fast as it is allowed to, and a site with three thousand browsers. ADR-0042 amendment A2 is the answer to the question "what happens after a week, at a hundred times the size, and when the client is rude?", and each part of it was decided by a measurement, not by a guess.

## 1. A limit that forgives
**What:** a *strike* is a mark against a connection that sent something wrong (a frame that is not Pusher, a signature that does not verify, too many frames). At `max_strikes` (8) the gateway closes it with 1008. **Why it needed work:** strikes were a lifetime count, so a tab open for a week that sent one malformed frame a day was closed on the eighth day. That punishes time, not abuse. **How:** one earlier strike is forgiven for each *full* 60 quiet seconds since the previous strike:

```php
$quiet = $now - $state->strikeStamp;          // negative if a clock went back: no forgiveness then
if ($quiet >= self::STRIKE_FORGIVE_SECONDS) {
    $state->strikes = max(0, $state->strikes - (int) floor($quiet / self::STRIKE_FORGIVE_SECONDS));
}
$state->strikes++;
```

The first version forgave a fraction (`quiet / 60`) and kept strikes as a float. Eight strikes a few milliseconds apart then summed to 7.9994 (5 ms apart), which is below 8, so a flood never reached the limit. The unit tests did not see it, because their fake clock stood still between strikes, so nothing was ever forgiven; the check against a real daemon did, because real frames arrive a few milliseconds apart. *Whole strikes, whole minutes* made a burst count exactly. The clock is `hrtime` (a *monotonic* clock: it only moves forward, whatever happens to the date on the machine), so a wall-clock step gives no credit and takes none away.

## 2. Numbers you can read, and nothing else
`Gateway::stats()` returns *gauges* (what is held now: connections, channels, joins in progress, presence members, memory) and *counters* (what has happened since the gateway was made: frames, joins, 4009 answers, strike closes and so on). `wsrv:echo` writes them as one line every 60 seconds (`--stats-interval`).

- **Numbers only.** The line is `name=value` pairs in a fixed order. A channel name, a user id or a socket id a browser sent can never reach a log line, so the line cannot carry injected text or personal data into a log you ship somewhere else.
- **Gauges return to zero.** When every browser has gone, every gauge must read zero. The churn test (random joins, answers, leaves and closes) asserts it, and so did the load run: nine load processes were killed with SIGKILL in the middle of a two-minute run, and afterwards every gauge was zero and the memory was flat. State a connection leaves behind shows up as a gauge that does not come back. That is a leak test that needs no profiler.
- **Counters only grow.** The difference between two readings is the rate. A counter that went down means the gateway was made again, that is, the worker lost the daemon and connected anew.
- **Compare with the daemon.** `connections` should match the daemon's `wsrv_connections_current`. It is *lower* for up to `shutdown_grace_ms` after a worker restart (a check showed 0 against 3), because the daemon still holds the previous worker's browsers and then closes them with 1012. A gauge that stays *above* the daemon's is the sign of a leak.

## 3. An error is not always a lost connection
A presence join is answered with the whole member list, as one frame to the daemon, and the daemon's IPC frame limit is `max_message_size` + 64 KiB. A list larger than that made `Client::send` throw a `ProtocolError`, and the `Supervisor` treats any `ProtocolError` as "the link is broken": it reconnects, and every browser is closed. One member with a large `user_info` could have taken the site down. The security review of the branch found it as an *availability* bug (not a way in, but a way to knock everyone out).

The fix has one idea: **make the error say what is wrong, so the caller can pick the size of the response.** `MessageTooLarge` is thrown *before any byte is written* (so the stream is still in step), the `Supervisor` reports it like a handler error and keeps the link, and the gateway fails *that join* with `pusher:subscription_error` (`ServerError`, "The member list is too large to send") and rolls it back: the daemon room, the member table, and no `member_added` was ever sent. A message, a join, a link: three sizes of failure, and now the right one is chosen.

The unit tests came first and were watched to fail. The check against a real daemon started with a small `max_message_size` came after the fix, and was watched to fail by putting the old behaviour back (a test you have never seen fail has not shown that it can). The guide gives the sizing rule: the list is about `members x (the JSON size of user_info + 40 bytes)`, and worst case with the defaults it is about 513 KB, because the list is JSON inside JSON: a quote or a backslash doubles twice and a U+2028 character grows from 3 to 7 bytes. (The first figure in this lesson, "a little over 400 KB", was wrong; a review rebuilt the worst case with the real encoder.)

**A second size limit that nobody had written down.** After the fix the guide still recommended `--max-message-size=16384`, and a review reproduced a join that got *no answer at all* with it: the daemon keeps at most `2 x max_message_size` bytes queued for one browser, so a message over `2 x 16384 - 16 = 32752` bytes could never be queued, and it was dropped *after* the daemon had told the worker "OK" (the status means "accepted for sending", and the size check happened later, on another thread). The same hole existed for a broadcast over the HTTP publish endpoint. The general lesson: *a limit has to be checked where the caller can still be told*. The cure was to check the size where the command comes in (the send and the publish are refused with an error), and to cap the frame limit the daemon announces to the largest message it can send, so that the worker's own client refuses first and the join fails with a clear error, the same way as above (ADR-0043). The fix for the default was the odd one: with no `--max-message-size` the queue was exactly as big as the biggest message, but with the flag set to the very same value it was twice as big. Two ways to say the same thing behaved differently, and a test that sets the flag could never have found it.

## 4. Defaults are for the biggest honest user
The daemon's default broker bounds (`broker_max_topics` 1 024 and the others) are fine for a chat room. The load run, 3 000 browsers with four channels each, had 3 918 of 12 000 joins refused: a topic exists only while it has subscribers, so a channel per user (`private-user.7`) is one topic per user. Nobody would have found that by reading; the load harness did, and the guide now has a sizing table. The `daemon_refused` counter turns "joins are failing" into "the daemon is too small for this site", which is a different page for the person on call than "a client is misbehaving".

## 5. A cache is three bounds and a guard
**The problem.** Pusher, and Reverb, answer a repeated `pusher:subscribe` for a presence channel the browser is already in with the whole member list again. `pusher-js` never asks twice, but any other client can, as fast as its message rate allows, and the answer costs time in proportion to the members. Measured with 100 members of 1.8 KB: 0.252 ms for each repeat.

**The design.** Keep the *encoded frame* for repeats only (a first join changes the list at once, so keeping it helps nobody). To know when it is out of date, `PresenceTable::revision()` returns a number that changes exactly when a channel's member list does (a user comes, or a user's last connection leaves; a second tab changes nothing) and is never used twice. It counts changes across *all* channels, rather than per channel, so a room that empties and is made again cannot restart at a number an old kept frame still carries. A repeat is answered from the kept frame only if the revision matches. Result: 0.006 ms.

**Three bounds, because "bounded" has three meanings.** Count: at most 16 frames, oldest out first. Size: a list over 1 MiB is answered each time but not kept, so the cache holds 16 MiB at most (a site that raises `max_channel_data_bytes` and `max_presence_members` can make a list tens of megabytes). Lifetime: a room's entry is dropped the moment its members change, so the `user_info` of people who left does not sit in memory. My first version had only the first bound; reading it again as "what does this hold, and for how long?" found the other two.

**A guard, not just a drop.** Dropping on every change depends on every code path that changes the members remembering to drop. The revision does not: even if a future path changes the table and forgets, a stale frame cannot be sent. One test changes the table *behind the gateway's back* to prove it.

**Mutation, and what a survivor means.** After the tests passed, each new line was broken on purpose, one at a time (27 breakages in all) and the tests had to fail. When the drop was added, two mutants survived: the revision check and the "newest last" ordering. A survivor is a question with two answers: the line is redundant (delete it), or nothing tests it (write the test). One line of the drop really was redundant and was deleted; the two guard lines got the test above.

**A bug found on the way.** Writing the revision test showed that `PresenceTable::remove()` for a connection that was *not* in a channel created an empty entry through a reference and then removed the whole channel. No caller could do that yet, since the gateway only removes what it added. It was fixed anyway, with a test, because "yet" is how a future bug starts.

## Pitfalls
- Forgiving in fractions, or timing a limit with the wall clock.
- Putting a string a browser sent into a statistics or log line.
- Catching a broad error type at the top and treating all of it as "the link is lost".
- Keying a cache by name only, without the version of the data it describes.
- Bounding a cache by the number of entries but not by what one entry can weigh, or by how long it may hold what it describes.
- Testing only with the sizes you expect: the defaults of lesson 56 were right for a small page, and the sizes of this lesson came from running a large one.

## Exercises
1. A tab sends one malformed frame every 90 seconds, forever. Is it ever closed? Another sends one every 30 seconds. When is it closed, with `max_strikes` 8? (Work through `strike()` for the first few.)
2. Eight strikes arrive 5 ms apart. With the first, fractional rule, what do they sum to? What does `floor` do instead, and why did the unit tests not see the difference?
3. The daemon's `max_message_size` is 1 MiB. Which limit decides whether a presence list is sent, and what is it in bytes? Which sizes of the three failures (message, join, link) does `MessageTooLarge` choose between?
4. Why does `PresenceTable` keep one counter for all channels instead of one per channel? Which test fails if you change it, and what would a client see?
5. Remove the line `unset($this->listFrames[$channel]);` from `succeededFrame()`. Which test fails now, and why did none fail before the guard test existed?
6. A site sets `max_channel_data_bytes` 65536 and `max_presence_members` 1000. Roughly how large can a list get (use `members x (2.33 x max_channel_data_bytes + 1 KB)`), and which two daemon limits must you raise for it? What does the gateway do with a repeated subscribe for it, given the 1 MiB cache bound?
7. `connections` reads 40 on the gateway and the daemon's `wsrv_connections_current` reads 12, and stays that way for a minute. What does it mean, and what is the first thing you look at? What if the gateway reads 0 and the daemon 40, five seconds after you restarted the worker?
