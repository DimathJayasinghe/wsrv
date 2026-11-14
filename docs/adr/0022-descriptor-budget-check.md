# ADR-0022: Descriptor budget check at start (RLIMIT_NOFILE)

- Status: **Accepted** (2026-11-14; M10 slice 1b; SECURITY_DESIGN T4, OPERATIONS section 4 "Start")
- Date: 2026-11-14
- Deciders: project owner, maintainer assistant

## Context
Every connection is one descriptor. A server configured for `max_connections = 10000` under a soft limit of 1024 works in every test and fails in production: `accept` returns `EMFILE`, the acceptor sheds load with its reserve descriptor (EC-N05), and the operator sees "connection refused" from a server that claims to allow 10000. SECURITY_DESIGN T4 and OPERATIONS section 4 already say the start must check `RLIMIT_NOFILE >= max_connections + reserve`; nothing does.

## Decision
1. **Needed descriptors** = `max_connections + 64 + 4 * threads`. The 64 covers the listener, the acceptor's reserve, the signal descriptor, stdio, log and certificate files and the embedding host's own; 4 per loop covers its epoll instance and wake-up eventfd with slack. The constants live in `server.cpp` next to the formula. Generous on purpose: a guess that is too small fails at start, one that is too large costs nothing. Both inputs are bounded by `validate()`, so the sum cannot overflow.
2. **`ensure_fd_budget(sys, needed)`** (`src/net/fd_budget.*`), called first in `Server::init`, before anything is bound:
   - soft limit already >= needed: nothing changes (no privilege used that is not needed);
   - else hard limit >= needed: raise the **soft limit to exactly `needed`** (not to the hard limit: least privilege, and the process keeps headroom for other code);
   - else, or when the kernel refuses the raise: fail the start with `ErrorCode::FdLimit` ("fail closed"). The server never starts in a state it knows to be unable to honour its own cap.
   The server logs the numbers (needed, soft, hard) on failure and the new value after a raise, because the error context is a fixed string.
3. **Interface:** `ISyscalls::get_fd_limits` / `set_fd_soft_limit` in plain integers (`FdLimits`; unlimited = the largest `uint64`). `set_fd_soft_limit` leaves the hard limit untouched. The fake can pretend a limit in memory (never touches the real process) so tests describe any host.
4. Embedded and `wsrvd` use the same path: a host that links the library gets the check at `wsrv_server_start`, and its limit is raised in the host process (documented in OPERATIONS).

## Alternatives
- Warn and continue: rejected, it is exactly the failure this ADR exists to prevent.
- Raise to the hard limit: rejected, more privilege than needed and it changes the limit for every other library in the process.
- Compute from the kernel's `fs.nr_open`: rejected, the hard limit is what the kernel actually enforces for this process.

## Consequences
+ A wrong `ulimit -n` or `LimitNOFILE=` is a start-up error with the three numbers needed to fix it.
- A process that must run with a small descriptor limit has to lower `max_connections`; that is the point.
- Raising the soft limit is a process-wide side effect of starting a server (documented; it only ever goes up to what the configuration needs).
