# ntsync mutexes are never abandoned once their handles are closed

| | |
|---|---|
| Patches | commits 4 and 5 on [`jegglest/wine`, branch `je-11.0`](https://github.com/jegglest/wine/commits/je-11.0) (`server/inproc_sync.c`, `server/handle.c`, `server/process.c`, `server/protocol.def`, `dlls/ntdll/unix/sync.c`, `dlls/ntdll/unix/server.c`) |
| Patch pages | [patch 4](patches/ntsync-mutex-owned-after-close.md) (`wineserver` only) and [patch 5](patches/ntsync-mutex-in-use-after-close.md) (the wait-all case, with the protocol change): the commit, the patch file, what each needs and how to check it |
| Upstream | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) (wineserver), filed 2026-10-01, UNCONFIRMED, no reply yet |
| Seen as | the two long-standing `kernel32:sync` `test_mutex` failures when `/dev/ntsync` is in use; a program that relies on `WAIT_ABANDONED` to recover from a dead thread hangs instead |
| Affects | Wine and Proton with ntsync (Linux 6.14 and later); the server-side mutexes used without ntsync are fine |
| Windows | abandons the mutex, measured on Windows 11 with [`repro/mutex_abandon.c`](repro/mutex_abandon.c) |

## Summary

With in-process synchronization on `/dev/ntsync`, a thread that dies while owning a mutex does not abandon it if
the process has already closed its handles to the mutex, even though another thread is still waiting on it. The
waiter blocks forever instead of returning `WAIT_ABANDONED`. Windows returns `WAIT_ABANDONED`, and so does Wine's
server-side implementation without ntsync. Wine's own test suite covers this and fails. The bug is in
`wineserver`'s bookkeeping, not in either architecture's `ntdll`; it was found while running `kernel32:sync` to
check the [stale thread alert](stale-thread-alert.md) patches and is otherwise unrelated to them.

Patch 4 keeps an owned mutex reachable after its server object is gone; patch 5 covers a mutex that is closed
while another thread of the process is still using it, which needs a new server request.

## Symptom

The end of `test_mutex()` in `dlls/kernel32/tests/sync.c` creates a mutex, has one thread acquire it and block,
starts a second thread waiting on the mutex, closes the mutex handle, terminates the owner with `TerminateThread()`
and expects the waiter to report `WAIT_ABANDONED` within a second. On a kernel with `/dev/ntsync`, `kernel32:sync`
fails the two checks that depend on that:

```
sync.c:416: Test failed: got 0x102
sync.c:420: Test failed: got 0x102
```

(`0x102` is `WAIT_TIMEOUT`; line numbers as of Wine master on 2026-09-30, 393 and 397 in Proton Experimental's
tree: the wait for the waiter to signal, and the wait for the waiter thread to exit.) Without `/dev/ntsync`, or
with `PROTON_NO_NTSYNC=1`, the test passes. The same test binary passes on Windows 11. Keeping the mutex handle open
until after `TerminateThread()` makes the ntsync case work too, so the failure is confined to closed handles.

## Cause

`server/inproc_sync.c` and `dlls/ntdll/unix/sync.c`:

- A client that waits on an in-process object holds only a duplicate of the object's ntsync file descriptor,
  received through the `get_inproc_sync_fd` request; it holds no server handle and no reference to the server
  object. The client-side cache keeps that descriptor open while the wait is in progress. (The comment above
  `struct inproc_sync` in `dlls/ntdll/unix/sync.c` describes the object outliving its handle by way of the client
  holding a handle, but nothing holds one.)
- So when the process closes its last handle, the server's mutex object is destroyed right away, and
  `inproc_sync_destroy()` takes the in-process sync off the `inproc_mutexes` list and closes the server's
  descriptor.
- When a thread dies, `abandon_inproc_mutexes()` sends `NTSYNC_IOC_MUTEX_KILL` to every mutex on that list, and
  only to those. The kernel object is still alive through the waiter's descriptor and still records the dead
  thread as its owner, but no kill reaches it. The driver abandons a mutex only through that ioctl on that
  object, so the mutex stays owned by a thread id that no longer exists and the waiter never wakes.

Upstream Wine master has the same code. On Windows the kernel keeps the mutants a thread owns on the thread
object, so termination abandons them whatever handles exist: "If a thread terminates without releasing its
ownership of a mutex object, the mutex object is considered to be abandoned. A waiting thread can acquire
ownership of an abandoned mutex object, but the wait function will return WAIT_ABANDONED to indicate that the
mutex object is abandoned" ([Mutex Objects](https://learn.microsoft.com/en-us/windows/win32/sync/mutex-objects)).

A second, narrower gap: a mutex that is unowned when its last handle is closed is forgotten just the same, and a
wait-all that was already pending on it can acquire it later (when the other objects of the wait are signaled).
If that thread then dies, the mutex is again not abandoned, and a second pending waiter blocks forever. Windows
abandons it there too.

## The fix

- **Patch 4**, `server: Keep an owned ntsync mutex reachable after its object is destroyed.` In
  `inproc_sync_destroy()`, when the sync is a mutex and `NTSYNC_IOC_MUTEX_READ` reports an owner, the server
  keeps the descriptor on a list of orphaned mutexes instead of closing it. `abandon_inproc_mutexes()` sends the
  kill to the orphans as well as to the live list, then reads each orphan and closes the ones that are no longer
  owned; the same pruning runs when an orphan is added, so the list only ever holds mutexes that were closed while
  owned and whose owner has neither released them nor died since. Unowned mutexes are closed as before, so the
  common case is unchanged. This fixes the test.
- **Patch 5**, `server: Keep an in-process mutex that a process closed while still using it.` For the wait-all
  gap. The client-side cache entry of an in-process sync counts its users, so `NtClose()` (and
  `NtDuplicateObject()` with `DUPLICATE_CLOSE_SOURCE`) can tell when another thread is still using a mutex. It then
  passes the entry's serial number with the `close_handle` or `dup_handle` request; the server keeps a reference
  to the in-process sync under that serial, which keeps the mutex on the list it kills at thread death; the
  entry's last user drops it with the new `release_inproc_sync` request, and a process's records go away with the
  process. Handles closed with no other user, the common case, involve no extra work. This is the design the
  comment in `dlls/ntdll/unix/sync.c` describes, done with a serial number instead of a handle. Not covered: users
  of uncached entries (a handle number reused while the old object is still in use, or several hundred thousand
  open handles), which `NtClose()` cannot see.

Patch 5 adds a field to two requests and one new request, so the server protocol version changes (Proton's
build regenerates the protocol headers from `server/protocol.def` with `tools/make_requests`). The new request is
appended after all existing ones, so request numbers do not change, but every unix-side `ntdll.so` must come
from the same tree as `wineserver`: in a full build like this one that is automatic. For a build that only replaces files, both the 64-bit and the 32-bit unix `ntdll` have to be
rebuilt, or 32-bit processes cannot connect to the server; Space Marine 2, for one, starts through a 32-bit
launcher stub on every launch even though the game is 64-bit.

## Testing

### Test program

[`repro/mutex_abandon.c`](repro/mutex_abandon.c) is a timed version of the test's sequence with three modes:

- `close`: a thread owns a mutex and blocks; a second thread waits on the mutex; the mutex handle is closed; the
  owner is killed with `TerminateThread()`. The waiter must get `WAIT_ABANDONED`. This is what the Wine test does.
- `keep`: the same with the handle kept open. Works everywhere; shows the failure is about closed handles.
- `waitall`: the mutex is unowned when its handle is closed, and two threads are blocked in wait-all calls on it,
  each with its own unset event. Setting event A lets thread A take the mutex; A is then killed; setting event B
  must give thread B the abandoned mutex (`WAIT_ABANDONED_0`).

It prints each step with a timestamp and reports `failed step(s)` for waits that did not return within 5 seconds.
Build and usage: [`repro/README.md`](repro/README.md). The Windows machine is the same Windows 11 VM as for the
other issue, on the same host, with the same binaries.

### Results

| Server | `close`: the waiter's mutex wait after the owner is killed | `keep` | `waitall` |
|---|---|---|---|
| Stock Proton Experimental 20260924, 64- and 32-bit programs | never returns (steps time out at 5 s) | `WAIT_ABANDONED` at once | never returns |
| Stock Proton Experimental 20261001, 64-bit program | never returns (6 failed steps in 3 iterations) | not run | not run |
| Patch 4 only | `WAIT_ABANDONED` within 0.1 ms, 25 of 25 iterations | at once | never returns (the mutex was unowned when forgotten) |
| Patches 4–5 on 20260924, 64- and 32-bit programs | `WAIT_ABANDONED` within 0.1 ms, 30 of 30 per architecture | at once | B gets `WAIT_ABANDONED_0` as soon as its event is set, 30 of 30 per architecture |
| Patches 4–5 on 20261001, 64- and 32-bit programs | `WAIT_ABANDONED`, 10 of 10 per architecture | at once, 10 of 10 | B gets `WAIT_ABANDONED_0`, 10 of 10 per architecture |
| Windows 11 25H2 | `WAIT_ABANDONED` 0.1 to 0.7 ms after the terminate, 5 of 5 | at once, 5 of 5 | B gets `WAIT_ABANDONED_0` as soon as its event is set, 3 of 3 |

The server's file-descriptor count was checked before and after the runs so that the orphan and serial lists do
not leak: unchanged after 80 iterations of all three modes (712 descriptors, 455 of them ntsync, on both bases).
Raw Windows output: [`results/windows-mutex-2026-09-30.txt`](results/windows-mutex-2026-09-30.txt).

Wine's conformance tests, same binaries on both systems:

| | `kernel32:sync` | `ntdll:sync` |
|---|---|---|
| Stock Proton Experimental 20260924 / 20261001 | 2 failures (`test_mutex`) | see the [other issue](stale-thread-alert.md) |
| Patch 4 | 0 failures (2 runs) | unchanged |
| Patches 1–5 | 0 failures (4 runs on 20260924, 2 on 20261001) | 441 tests, 0 failures |
| Windows 11 | 0 failures | 0 failures |

Nothing in Space Marine 2 is known to depend on this; the game runs as before on builds with patches 4–5, and the
32-bit launcher stub it starts with runs fine on the rebuilt 32-bit unix `ntdll`.

## Upstream status

- 2026-10-01: Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) filed, with the cause, both parts of
  the fix in prose, the Windows measurements and the 32-bit findings. No reply as of 2026-10-07; UNCONFIRMED.
- 2026-10-01: the fix and its probe posted on Proton
  [#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943056744) (server diff of patches
  4–5 in a details block), and a [note](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943989743)
  linking the Wine bug.
- As with the other issue, the patches were written with an LLM and are a reference for upstream, not a merge
  request.

## References

- Wine bug 60417: https://bugs.winehq.org/show_bug.cgi?id=60417
- Wine's `test_mutex()`: https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/kernel32/tests/sync.c
- Mutex objects on Windows: https://learn.microsoft.com/en-us/windows/win32/sync/mutex-objects
- ntsync driver: https://docs.kernel.org/userspace-api/ntsync.html
