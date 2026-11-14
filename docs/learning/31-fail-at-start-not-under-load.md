# Lesson 31 — Fail at start, not under load (the descriptor budget, M10 slice 1b)

## Goal
After this lesson you can explain what `RLIMIT_NOFILE` is, why a server must compare it with its own connection cap before it binds, why the soft limit is raised only as far as needed, and how a test can describe any host limit without touching the real process.

## Background: a limit nobody reads until it bites
Every open socket, file, epoll instance and eventfd is a *file descriptor* (a small integer the kernel gives a process). A process may hold only so many: the **soft limit** is the one enforced (`open`/`accept` fail with `EMFILE` beyond it); the **hard limit** is the ceiling an unprivileged process may raise the soft limit to. Distributions often ship a soft limit of 1024. A server configured for 10 000 connections then works in every test and accepts connection number 1 021 with `EMFILE`; the reserve-descriptor trick (EC-N05) lets it shed the overflow politely, but the operator still sees a server that silently allows a tenth of what the config says.

## Design
Check **once, first, before binding** (ADR-0022): needed = `max_connections + 64 + 4 * threads`. Soft limit enough: leave it. Soft too low but hard enough: raise soft to exactly `needed` (least privilege; the hard limit and other libraries' headroom stay as they are). Hard too low, or the kernel refuses: fail the start. The error code is `FdLimit`; the numbers go to the log, because an error context in this code base is a fixed string and never built at runtime.

The check sits behind `ISyscalls` (two new calls in plain integers; "unlimited" is the largest `uint64`, so one comparison is always right). The fake can **pretend** a limit in memory (`pretend_fd_limits(soft, hard)`), so the tests describe a 256/256 host, a 1024/1 048 576 host, or an unlimited one without ever lowering the limit of the test process, which would break every later test in it.

## Walkthrough
1. `FdBudget.AnAlreadySufficientSoftLimitIsLeftAlone`: no `setrlimit` call is made (the fake counts calls).
2. `ALowSoftLimitIsRaisedToWhatIsNeededAndNotToTheHardLimit`: after the check, soft is exactly `needed` and hard is unchanged.
3. `AHardLimitBelowTheNeedIsAClearRefusal` and `ARefusedRaiseIsARefusalNotASilentContinue`: both end in `FdLimit`, and the first never even tries to raise.
4. `ServerBuilder.ADescriptorLimitBelowTheCapRefusesToStartBeforeBinding`: the whole server refuses, `bind` was never called, the log line has both numbers.
5. Mutation check: replace the call by "always fine" and both server tests go red, so they test the wiring and not just the helper.

## Pitfalls
- A fake that is *kinder* than the kernel. The first version of the fake answered `EPERM` for "soft above hard"; the real kernel answers `EINVAL` (we keep the hard limit, so soft > hard is simply invalid), and a test against the real kernel caught the difference. A test double must copy the kernel's answers, not our expectations of them.
- Raising to the hard limit "to be safe": it hands a process-wide change to code that never asked for it.
- Reading the limit later "when needed": by then the failure is `EMFILE` in the accept path.

## Exercises
1. A deployment under systemd has `LimitNOFILE=4096` and `max_connections = 10000`. What does the operator see, and which two numbers could they change?
2. Why is `4 * threads` added and not `4`? What happens at `threads = 256`?
3. The check raises a process-wide limit. Name one consequence for a PHP process that embeds the library, and where OPERATIONS says so.
