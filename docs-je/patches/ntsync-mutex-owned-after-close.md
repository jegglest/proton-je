# Patch 4: keep an owned ntsync mutex until its owner releases it or dies

| | |
|---|---|
| Commit | [`ntsync-mutex-owned-after-close`](https://github.com/jegglest/wine/commit/ntsync-mutex-owned-after-close) `server: Keep an owned ntsync mutex until its owner releases it or dies.` ([patch file](https://github.com/jegglest/wine/commit/ntsync-mutex-owned-after-close.patch)); the tag moves to the rebased commit with every new Proton Experimental |
| Changes | `server/inproc_sync.c`, 49 lines added and 1 removed: a list of orphaned mutexes, used by `inproc_sync_destroy()` and `abandon_inproc_mutexes()` |
| Fixes | with `/dev/ntsync`, a thread that dies while owning a mutex whose handles its process has already closed never abandons it, and a thread waiting on the mutex blocks forever instead of getting `WAIT_ABANDONED`. The two `kernel32:sync` `test_mutex` failures under ntsync: the check is Wine's own, added with `934514db7c2` (Wine 10.12) when Wine made its server-side mutexes do this, and it has failed with ntsync since in-process synchronization arrived in Wine 10.16 (`7bb835b3482`) |
| Needs | nothing. `wineserver` only, no protocol change: replacing `wineserver` alone is enough |
| Applies to | checked 2026-10-28: Valve's Wine [`7fc704ef`](https://github.com/ValveSoftware/wine/commit/7fc704ef49c50e9702c9983a5250d5738697bb04) of Experimental 20261014, Valve's bleeding-edge branch (`674d3cdd`) and wine-cachyos (`cachyos_11.0_20261014/main`) with `git am`, no conflicts (earlier: `6d211aab`, `bc8d17c6`, wine-cachyos of 2026-10-04). Wine master (`66caeb46`): the hunks collide with Proton's fsync fallback, which sits on the same lines of `inproc_sync_destroy()` and `abandon_inproc_mutexes()` in Valve's tree and is absent upstream; the logic ports unchanged |
| Upstream | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) (wineserver), filed 2026-10-01, UNCONFIRMED, no reply as of 2026-10-28 |
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
mutex without ntsync or fsync. Proton's fsync fallback shares this bookkeeping and has the same gap; the patch's
ntsync-only branches do not cover it.

Wine's own test covers this: the end of `test_mutex()` in `dlls/kernel32/tests/sync.c` closes the mutex handle,
terminates the owner and expects the waiter to report `WAIT_ABANDONED` within a second. With `/dev/ntsync`:

```
sync.c:416: Test failed: got 0x102
sync.c:420: Test failed: got 0x102
```

## The change

Wine has done this for its server-side mutexes since Rémi Bernon's `934514db7c2`, "server: Keep owned mutex
syncs alive until abandoned." (Wine 10.12): an owned mutex holds a reference to itself until it is released or
abandoned, and the end of `test_mutex()` checks exactly that. An in-process mutex cannot do the same, because the
server never sees an in-process acquisition or release; the moment its object is destroyed is the one point where
the server can look.

When the sync being destroyed is a mutex that `NTSYNC_IOC_MUTEX_READ` reports as owned, the server keeps its
descriptor on a list of orphaned mutexes instead of closing it. `abandon_inproc_mutexes()` sends the kill to
the orphans as well as to the live list, then reads each orphan and closes the ones that are no longer owned;
the same pruning runs when an orphan is added. So the list only ever holds mutexes that were closed while owned
and whose owner has neither released them nor died since. Unowned mutexes are closed as before, so the common
path is unchanged.

## How to check it

`mutex_abandon.exe close` ([`repro/`](../repro)): a thread owns a mutex and blocks, a second thread waits on it,
the handle is closed, the owner is killed with `TerminateThread()`; the waiter must get `WAIT_ABANDONED`. It
needs `/dev/ntsync` in use (Linux 6.14 or later). Under the `proton` script `PROTON_NO_NTSYNC=1` alone falls back to
fsync, which has the same gap; with `PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1` the server-side mutexes are used and the
bug does not occur.

| | `close` |
|---|---|
| Stock Proton Experimental 20260924 and 20261001, 64- and 32-bit programs | never returns; the steps time out at 5 s |
| Patch 4 | `WAIT_ABANDONED` within 0.1 ms, 25 of 25 iterations |
| Windows 11 25H2 | `WAIT_ABANDONED` 0.1 to 0.7 ms after the terminate, 5 of 5 |

`kernel32:sync`: 2 failures on stock, 0 with the patch (2 runs), 0 on Windows with the same binary. Again with
the current commits on Proton Experimental 20261014, with this patch alone (stock `ntdll`, a `wineserver` with only
patch 4): `test_mutex` passes in 3 of 3 runs, `mutex_abandon close` 3 of 3, and only patch 6's wait-all test
fails, which is the case patch 5 is for. The server's
descriptor count was checked before and after 80 iterations of the probe's three modes and did not change, so
the orphan list does not leak. The probe's `keep` mode (handle kept open) works everywhere and shows that the
failure is confined to closed handles; its `waitall` mode still fails with this patch alone, and is what
[patch 5](ntsync-mutex-in-use-after-close.md) is for.

## Upstream

- 2026-10-01: Wine bug 60417 filed with the cause, both parts of the fix in prose, the Windows measurements and
  the 32-bit findings; no reply as of 2026-10-28. On 2026-10-17 its second part got its own report, bug
  [60524](https://bugs.winehq.org/show_bug.cgi?id=60524) (patches 5 and 6).
- 2026-10-01: the server diff of patches 4 and 5 and the probe posted on Proton
  [#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943056744).
- Written with an LLM; a reference for upstream, not a merge request.
