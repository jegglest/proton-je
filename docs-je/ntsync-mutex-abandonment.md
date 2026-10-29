# ntsync mutexes are never abandoned once their handles are closed

| | |
|---|---|
| Patches | commits 4 to 6 on [`jegglest/wine`, branch `je-11.0`](https://github.com/jegglest/wine/commits/je-11.0) (`server/inproc_sync.c`, `server/process.c`, `server/object.h`, `server/protocol.def`, `dlls/ntdll/unix/sync.c`; the test in `dlls/kernel32/tests/sync.c`) |
| Patch pages | [patch 4](patches/ntsync-mutex-owned-after-close.md) (`wineserver` only) and [patches 5 and 6](patches/ntsync-mutex-in-use-after-close.md) (the wait-all case, with two new server requests, and its test): the commit, the patch file, what each needs and how to check it |
| Upstream | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) (wineserver), filed 2026-10-01, and bug [60524](https://bugs.winehq.org/show_bug.cgi?id=60524), its second part, filed 2026-10-17; both UNCONFIRMED, no reply as of 2026-10-28 |
| Seen as | the two long-standing `kernel32:sync` `test_mutex` failures when `/dev/ntsync` is in use; a program that relies on `WAIT_ABANDONED` to recover from a dead thread hangs instead |
| Affects | Wine and Proton with ntsync (Linux 6.14 and later), and Proton's fsync fallback, which shares the bookkeeping and is not covered by these patches; the server-side mutexes used without ntsync and fsync (`PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1` under Proton) are fine |
| Windows | abandons the mutex, measured on Windows 11 with [`repro/mutex_abandon.c`](repro/mutex_abandon.c) |

## Summary

With in-process synchronization on `/dev/ntsync`, a thread that dies while owning a mutex does not abandon it if
the process has already closed its handles to the mutex, even though another thread is still waiting on it. The
waiter blocks forever instead of returning `WAIT_ABANDONED`. Windows returns `WAIT_ABANDONED`, and so does Wine's
server-side implementation without ntsync or fsync. Wine's own test suite covers this and fails. The bug is in
`wineserver`'s bookkeeping, not in either architecture's `ntdll`; it was found while running `kernel32:sync` to
check the [stale thread alert](stale-thread-alert.md) patches and is otherwise unrelated to them.

Patch 4 keeps an owned mutex reachable after its server object is gone; patch 5 covers a mutex that is closed
while another thread of the process is still using it, with two new server requests; patch 6 tests that case.
The failing check is Wine's own: Rémi Bernon added it with `934514db7c2`, "server: Keep owned mutex syncs alive
until abandoned." (Wine 10.12), which made Wine's server-side mutexes do what patch 4 does for in-process ones,
and it has failed with ntsync since in-process synchronization arrived in Wine 10.16 (`7bb835b3482`).

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
tree: the wait for the waiter to signal, and the wait for the waiter thread to exit.) On the server-side mutexes
the test passes; under the `proton` script those need `PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1`, because `PROTON_NO_NTSYNC=1`
alone, or a kernel without `/dev/ntsync`, falls back to fsync, which hangs the same way (checked with the reproducer
below on release `-3`, 2026-10-12). The same test binary passes on Windows 11. Keeping the mutex handle open
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

- **Patch 4**, `server: Keep an owned ntsync mutex until its owner releases it or dies.` In
  `inproc_sync_destroy()`, when the sync is a mutex and `NTSYNC_IOC_MUTEX_READ` reports an owner, the server
  keeps the descriptor on a list of orphaned mutexes instead of closing it. `abandon_inproc_mutexes()` sends the
  kill to the orphans as well as to the live list, then reads each orphan and closes the ones that are no longer
  owned; the same pruning runs when an orphan is added, so the list only ever holds mutexes that were closed while
  owned and whose owner has neither released them nor died since. Unowned mutexes are closed as before, so the
  common case is unchanged. This fixes the test.
- **Patch 5**, `server: Keep an in-process mutex a client closed while still using it.` For the wait-all
  gap. The client-side cache entry of an in-process sync counts its users, so `close_inproc_sync()`, which
  `NtClose()` and `NtDuplicateObject()` with `DUPLICATE_CLOSE_SOURCE` call before their server request, can tell
  when another thread is still using a mutex. It then sends the new `keep_inproc_sync` request with the handle and
  the entry's serial number, before the handle is closed; the server keeps a reference to the in-process sync
  under that serial, which keeps the mutex on the list it kills at thread death; the entry's last user drops it
  with the new `release_kept_inproc_sync` request, and a process's records go away with the process. Handles
  closed with no other user, the common case, involve no extra request. This is the design the comment in
  `dlls/ntdll/unix/sync.c` describes, done with a serial number instead of a handle. Not covered: users of
  uncached entries (a handle number reused while the old object is still in use, or several hundred thousand
  open handles), which `close_inproc_sync()` cannot see. The serial makes the cache entry 20 bytes, so the
  cache's 64K blocks no longer divide into whole entries; the patch maps whole blocks. Its first version mapped
  the entries' exact size, 65520 bytes, which `anon_mmap_alloc()` rejects with an assertion once a process waits
  on a handle past the first block of 3276 entries: that is the abort releases `-1` and `-2` had, found with
  Foundry ([details](patches/ntsync-mutex-in-use-after-close.md#the-change)). Until release `-3` the serial
  travelled in the `close_handle` and `dup_handle` requests instead; release `-4` sends the keep as a request of
  its own, so no existing request and neither close path changes.
- **Patch 6**, `kernel32/tests: Test abandoning a mutex acquired after it was closed.` The probe's `waitall`
  sequence as a conformance test in `kernel32:sync`: it fails with ntsync without patch 5, on a timeout rather
  than a hang, and passes everywhere else, Windows 11 included.

Patch 5 appends two requests to `server/protocol.def` and changes no existing one, so request numbers do not
change. For the keep to happen, every unix-side `ntdll.so` must come from the same tree as `wineserver`, which is
automatic in a full build like this one; for a build that only replaces files, both the 64-bit and the 32-bit
unix `ntdll` have to be rebuilt along with the server (Space Marine 2, for one, starts through a 32-bit launcher
stub on every launch even though the game is 64-bit). A mismatched pair does no harm beyond that: a server
without the requests answers them with "not implemented" and the mutex is forgotten as before. Proton's build
regenerates the protocol headers from a stale committed copy with `tools/make_requests`, so stock and patched
builds report the same protocol version number and the version check does not tell them apart.

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
| Release `-3` on the fsync fallback (`PROTON_NO_NTSYNC=1 WINEFSYNC=1`), 64-bit program | never returns (4 failed steps in 2 iterations) | not run | never returns (4 failed steps in 2 iterations) |
| Release `-3` on the server-side mutexes (`PROTON_NO_NTSYNC=1`, no `WINEFSYNC`), 64-bit program | `WAIT_ABANDONED` within 0.5 ms, 2 of 2 | not run | B gets `WAIT_ABANDONED_0` as soon as its event is set, 2 of 2 |
| Patch 4 only | `WAIT_ABANDONED` within 0.1 ms, 25 of 25 iterations | at once | never returns (the mutex was unowned when forgotten) |
| Patches 4–5 on 20260924, 64- and 32-bit programs | `WAIT_ABANDONED` within 0.1 ms, 30 of 30 per architecture | at once | B gets `WAIT_ABANDONED_0` as soon as its event is set, 30 of 30 per architecture |
| Patches 4–5 on 20261001, 64- and 32-bit programs | `WAIT_ABANDONED`, 10 of 10 per architecture | at once, 10 of 10 | B gets `WAIT_ABANDONED_0`, 10 of 10 per architecture |
| Patches 4–6 of release `-4` on 20261014, 64- and 32-bit programs | `WAIT_ABANDONED`, 3 of 3 per architecture | at once, 2 of 2 (64-bit) | B gets `WAIT_ABANDONED_0`, 3 of 3 per architecture |
| Patch 5 alone on 20261014 (patch 4's orphan list disabled), 64-bit | `WAIT_ABANDONED`, 3 of 3 | not run | B gets `WAIT_ABANDONED_0`, 3 of 3 |
| Windows 11 25H2 | `WAIT_ABANDONED` 0.1 to 0.7 ms after the terminate, 5 of 5 | at once, 5 of 5 | B gets `WAIT_ABANDONED_0` as soon as its event is set, 3 of 3 |

The server's file-descriptor count was checked before and after the runs so that the orphan and serial lists do
not leak: unchanged after 80 iterations of all three modes (712 descriptors, 455 of them ntsync, on both bases).
Raw Windows output: [`results/windows-mutex-2026-09-30.txt`](results/windows-mutex-2026-09-30.txt). With release
`-4`'s form of patch 5, a persistent server's descriptor count stayed at 712 over 30 `waitall` and 30 `close`
probe processes and one process running 20 `waitall` iterations, each of which goes through a keep and a release.

The handle-cache probe ([`repro/inproc_cache_probe.c`](repro/inproc_cache_probe.c), 5000 events waited on once each),
added after the cache block slip described above: release `-2` aborts it at the wait on the handle with index 3276
and then hangs, while 3000 events pass there; with the fixed unix `ntdll.so` built from commit `7dda1b8` and swapped
into a copy of `-2`, 5000 and 20000 events pass, and `waitall` and `close` still abandon the mutex, 3 of 3 each.

Wine's conformance tests, same binaries on both systems:

| | `kernel32:sync` | `ntdll:sync` |
|---|---|---|
| Stock Proton Experimental 20260924 / 20261001 | 2 failures (`test_mutex`) | see the [other issue](stale-thread-alert.md) |
| Patch 4 | 0 failures (2 runs) | unchanged |
| Patches 1–5 | 0 failures (4 runs on 20260924, 2 on 20261001) | 441 tests, 0 failures |
| Stock Proton Experimental 20261014, with patch 6's test | 3 failures (`test_mutex` twice, `test_mutex_wait_all` once), 3 of 3 runs | see the [other issue](stale-thread-alert.md) |
| Patches 1–6 of release `-4` on 20261014 | 0 failures (5 runs 64-bit, 3 runs 32-bit; also without ntsync) | 437 tests, 0 failures |
| Patch 4 alone on 20261014, with patch 6's test | 1 failure (`test_mutex_wait_all`), 3 of 3 runs | unchanged |
| Windows 11 | 0 failures (patch 6 included: 64- and 32-bit, 3 runs each, 2026-10-28) | 0 failures |

Nothing in Space Marine 2 is known to depend on this; the game runs as before on builds with patches 4–5, and the
32-bit launcher stub it starts with runs fine on the rebuilt 32-bit unix `ntdll`.

## Upstream status

- 2026-10-01: Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) filed, with the cause, both parts of
  the fix in prose, the Windows measurements and the 32-bit findings. No reply as of 2026-10-28; UNCONFIRMED.
- 2026-10-17: the second part filed on its own as Wine bug [60524](https://bugs.winehq.org/show_bug.cgi?id=60524), linking patch 5. UNCONFIRMED.
- 2026-10-01: the fix and its probe posted on Proton
  [#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943056744) (server diff of patches
  4–5 in a details block), and a [note](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943989743)
  linking the Wine bug.
- As with the other issue, the patches were written with an LLM and are a reference for upstream, not a merge
  request.
- For the record, Windows needs none of this bookkeeping: the NT kernel lists the mutexes a thread owns in the
  thread itself and abandons that list when the thread is run down, and a wait references the object, not the
  handle. The ntsync driver keeps no per-owner list and leaves owner death to userspace, one kill per mutex, which
  is why `wineserver` has to know every mutex a dying thread might own. A per-owner list in the driver would be
  the faithful fix; whether to propose it is an open question, noted on the
  [patch page](patches/ntsync-mutex-in-use-after-close.md#upstream) and not pursued yet.

## References

- Wine bug 60417: https://bugs.winehq.org/show_bug.cgi?id=60417
- Wine's `test_mutex()`: https://gitlab.winehq.org/wine/wine/-/blob/master/dlls/kernel32/tests/sync.c
- Mutex objects on Windows: https://learn.microsoft.com/en-us/windows/win32/sync/mutex-objects
- ntsync driver: https://docs.kernel.org/userspace-api/ntsync.html
- How the NT kernel does it, in ReactOS: `KiSatisfyMutantWait` (ownership recorded in the thread's mutant list),
  https://github.com/reactos/reactos/blob/master/ntoskrnl/include/internal/ke_x.h; `KeRundownThread` (the list
  abandoned at thread death), https://github.com/reactos/reactos/blob/master/ntoskrnl/ke/thrdobj.c;
  `NtWaitForMultipleObjects` (the wait references the objects), https://github.com/reactos/reactos/blob/master/ntoskrnl/ob/obwait.c
