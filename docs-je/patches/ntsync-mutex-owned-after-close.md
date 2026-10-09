# Patch 4: keep an owned ntsync mutex reachable after its server object is destroyed

| | |
|---|---|
| Commit | [`7562ac9bc0`](https://github.com/jegglest/wine/commit/7562ac9bc03a21d009a64f9094d00bb6439be1a3) `server: Keep an owned ntsync mutex reachable after its object is destroyed.` ([patch file](https://github.com/jegglest/wine/commit/7562ac9bc03a21d009a64f9094d00bb6439be1a3.patch)) |
| Changes | `server/inproc_sync.c`, 49 lines added and 1 removed: a list of orphaned mutexes, used by `inproc_sync_destroy()` and `abandon_inproc_mutexes()` |
| Fixes | with `/dev/ntsync`, a thread that dies while owning a mutex whose handles its process has already closed never abandons it, and a thread waiting on the mutex blocks forever instead of getting `WAIT_ABANDONED`. The two long-standing `kernel32:sync` `test_mutex` failures under ntsync |
| Needs | nothing. `wineserver` only, no protocol change: replacing `wineserver` alone is enough |
| Applies to | Valve's Wine [`6d211aab`](https://github.com/ValveSoftware/wine/commit/6d211aabd1d325c9990a5fd7a81b9db37caae12d) and wine-cachyos as of 2026-10-04 with `git am`, no conflicts. Wine master as of 2026-10-09: the hunks collide with Proton's fsync fallback, which sits on the same lines of `inproc_sync_destroy()` and `abandon_inproc_mutexes()` in Valve's tree and is absent upstream; the logic ports unchanged |
| Upstream | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) (wineserver), filed 2026-10-01, UNCONFIRMED, no reply as of 2026-10-09 |
| Windows | abandons the mutex: `WAIT_ABANDONED` 0.1 to 0.7 ms after `TerminateThread()`, 5 of 5 runs of [`repro/mutex_abandon.c`](../repro/mutex_abandon.c) |
| Full story | [ntsync mutexes are never abandoned once their handles are closed](../ntsync-mutex-abandonment.md) |

## The bug

A client waiting on an in-process object holds only a duplicate of its ntsync file descriptor, no server
handle and no reference to the server object. When the process closes its last handle, the server's mutex
object is destroyed right away: `inproc_sync_destroy()` takes it off the `inproc_mutexes` list and closes the
server's descriptor. When a thread dies, `abandon_inproc_mutexes()` sends `NTSYNC_IOC_MUTEX_KILL` to the mutexes
on that list and only to those. The kernel object is still alive through the waiter's descriptor and still
records the dead thread as its owner, but no kill reaches it, so the waiter never wakes. Windows keeps the
mutants a thread owns on the thread object and abandons them whatever handles exist; so does Wine's server-side
mutex without ntsync.

Wine's own test covers this: the end of `test_mutex()` in `dlls/kernel32/tests/sync.c` closes the mutex handle,
terminates the owner and expects the waiter to report `WAIT_ABANDONED` within a second. With `/dev/ntsync`:

```
sync.c:416: Test failed: got 0x102
sync.c:420: Test failed: got 0x102
```

## The change

When the sync being destroyed is a mutex that `NTSYNC_IOC_MUTEX_READ` reports as owned, the server keeps its
descriptor on a list of orphaned mutexes instead of closing it. `abandon_inproc_mutexes()` sends the kill to
the orphans as well as to the live list, then reads each orphan and closes the ones that are no longer owned;
the same pruning runs when an orphan is added. So the list only ever holds mutexes that were closed while owned
and whose owner has neither released them nor died since. Unowned mutexes are closed as before, so the common
path is unchanged.

## How to check it

`mutex_abandon.exe close` ([`repro/`](../repro)): a thread owns a mutex and blocks, a second thread waits on it,
the handle is closed, the owner is killed with `TerminateThread()`; the waiter must get `WAIT_ABANDONED`. It
needs `/dev/ntsync` in use (Linux 6.14 or later; with `PROTON_NO_NTSYNC=1` the server-side mutexes are used and
the bug does not occur).

| | `close` |
|---|---|
| Stock Proton Experimental 20260924 and 20261001, 64- and 32-bit programs | never returns; the steps time out at 5 s |
| Patch 4 | `WAIT_ABANDONED` within 0.1 ms, 25 of 25 iterations |
| Windows 11 25H2 | `WAIT_ABANDONED` 0.1 to 0.7 ms after the terminate, 5 of 5 |

`kernel32:sync`: 2 failures on stock, 0 with the patch (2 runs), 0 on Windows with the same binary. The server's
descriptor count was checked before and after 80 iterations of the probe's three modes and did not change, so
the orphan list does not leak. The probe's `keep` mode (handle kept open) works everywhere and shows that the
failure is confined to closed handles; its `waitall` mode still fails with this patch alone, and is what
[patch 5](ntsync-mutex-in-use-after-close.md) is for.

## Upstream

- 2026-10-01: Wine bug 60417 filed with the cause, both parts of the fix in prose, the Windows measurements and
  the 32-bit findings; no reply as of 2026-10-09.
- 2026-10-01: the server diff of patches 4 and 5 and the probe posted on Proton
  [#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943056744).
- Written with an LLM; a reference for upstream, not a merge request.
