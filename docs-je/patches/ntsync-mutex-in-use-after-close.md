# Patches 5 and 6: keep an in-process mutex a client closed while still using it

| | |
|---|---|
| Commits | [`ntsync-mutex-in-use-after-close`](https://github.com/jegglest/wine/commit/ntsync-mutex-in-use-after-close) `server: Keep an in-process mutex a client closed while still using it.` ([patch file](https://github.com/jegglest/wine/commit/ntsync-mutex-in-use-after-close.patch)); [`ntsync-mutex-in-use-after-close-test`](https://github.com/jegglest/wine/commit/ntsync-mutex-in-use-after-close-test) `kernel32/tests: Test abandoning a mutex acquired after it was closed.` ([patch file](https://github.com/jegglest/wine/commit/ntsync-mutex-in-use-after-close-test.patch)); the tags move to the rebased commits with every new Proton Experimental |
| Changes | patch 5: 5 files, 159 lines added and 5 removed: `server/inproc_sync.c`, `server/process.c`, `server/object.h`, `server/protocol.def` (two new requests, `keep_inproc_sync` and `release_kept_inproc_sync`, appended after the existing ones; no existing request changes), `dlls/ntdll/unix/sync.c`. Patch 6: `dlls/kernel32/tests/sync.c`, 87 lines added: `test_mutex_wait_all()` in `kernel32:sync` |
| Fixes | the case patch 4 leaves open: a mutex that is unowned when its last handle is closed, with a wait-all still pending on it, can be acquired later; if that thread then dies the mutex is again not abandoned and a second waiter blocks forever |
| Needs | [patch 4](ntsync-mutex-owned-after-close.md), textually. `wineserver` and both unix-side `ntdll.so` (64- and 32-bit) should come from the same tree, as they do in a full build: a server without the new requests answers them with "not implemented", and the mutex is then forgotten as before, nothing worse. Proton's build regenerates the protocol headers from a stale committed copy, so stock and patched builds report the same protocol version number |
| Applies to | checked 2026-10-28, with patch 4: Valve's Wine [`7fc704ef`](https://github.com/ValveSoftware/wine/commit/7fc704ef49c50e9702c9983a5250d5738697bb04) of Experimental 20261014, Valve's bleeding-edge branch (`674d3cdd`) and wine-cachyos (`cachyos_11.0_20261014/main`), both commits with `git am`. Wine master (`66caeb46`): patch 6 with `git am`; patch 5 needs a hand port, its hunks in `server/inproc_sync.c` and `dlls/ntdll/unix/sync.c` collide with Proton's fsync fallback next to the changed lines |
| Upstream | Wine bug [60524](https://bugs.winehq.org/show_bug.cgi?id=60524), filed 2026-10-17 from the second part of bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417); both UNCONFIRMED |
| Windows | abandons it: the second wait-all waiter gets `WAIT_ABANDONED_0` as soon as its event is set, 3 of 3 with the probe; patch 6 passes on Windows 11 25H2, 64- and 32-bit, 3 of 3 runs each |
| Full story | [ntsync mutexes are never abandoned once their handles are closed](../ntsync-mutex-abandonment.md) |

## The bug

Patch 4 keeps a mutex that is owned at the moment its server object is destroyed. A mutex that is unowned at
that moment is forgotten just the same, and a wait that was already pending on it, for instance a wait-all
whose other objects are signaled later, can still acquire it through the duplicate descriptor it holds. If that
thread then dies, nothing sends the kill, the mutex stays owned by a dead thread id, and another pending waiter
blocks forever. Windows abandons it there too.

## The change

The client-side cache entry of an in-process sync counts its users, so `close_inproc_sync()`, which `NtClose()`
and `NtDuplicateObject()` with `DUPLICATE_CLOSE_SOURCE` call before their server request, can tell when another
thread of the process is still using the mutex. It then sends the new `keep_inproc_sync` request with the handle
and the entry's serial number, before the handle is closed; the server keeps a reference to the in-process sync
under that serial, which keeps the mutex on the list it kills at thread death. The entry's last user drops it
with the new `release_kept_inproc_sync` request, and a process's records go away with the process. A handle
closed with no other user, the common case, sends no extra request. This is the design the comment above
`struct inproc_sync` in `dlls/ntdll/unix/sync.c` already describes, the client keeping the object until its last
user is done, made only when it is needed and with a serial number instead of a handle. Releases `-1` to `-3`
passed the serial in the `close_handle` and `dup_handle` requests instead, which needed `NtClose()` and
`NtDuplicateObject()` split around their requests; release `-4` sends the keep as a request of its own, so no
existing request and neither close path changes.

Not covered: users of uncached entries (a handle number reused while the old object is still in use, or
several hundred thousand open handles), which `NtClose()` cannot see.

The serial makes `struct inproc_sync` 20 bytes instead of 16. The cache keeps its entries in blocks of
`65536 / sizeof(struct inproc_sync)` of them, and the first version of this commit mapped exactly that many
entries for each block after the first, 65520 bytes, which `anon_mmap_alloc()` refuses with an assertion because
it is not a multiple of the page size. The commit now maps whole 64K blocks. The first block is static, so the
abort needs a process with more than 3276 handles open at once that then waits on one of the higher ones:
Foundry (Steam app 983870) got there during an autosave and died with one line in Steam's console log,
``virtual.c:408: anon_mmap_alloc: Assertion `!(size & host_page_mask)' failed.``, no crash dump and no
`winedbg` output. Releases `proton-je-11.0-20261001-1` and `-2` carry the unfixed version; `-3` has this one.

Patch 6 is the conformance test, `test_mutex_wait_all()` in `kernel32:sync`, in the style of the end of
`test_mutex()`: two threads each wait for the mutex and an event of their own in one wait-all, the mutex handle is
closed while both are blocked, the first thread gets the mutex when its event is set and is then terminated, and
the second must get `WAIT_ABANDONED_0` (or `+1`; the documentation only promises the range for a wait-all) when its
own event is set. Without patch 5 it fails with ntsync on a 5 s timeout instead of hanging; without ntsync it passes
everywhere. The test waits 100 ms before it terminates the first thread, which gives that thread time to block on
its last event: its first wait on a handle takes `get_inproc_sync()`'s slow path, which holds `fd_cache_mutex`
across a server request, and a thread terminated during a server request leaves that lock taken for good. That is
a hazard of Wine's own, not of these patches; `test_mutex()` has the same margin.

## How to check it

`mutex_abandon.exe waitall` ([`repro/`](../repro)): the mutex is unowned when its handle is closed and two
threads are blocked in wait-all calls on it, each with its own unset event. Setting event A lets thread A take
the mutex; A is killed; setting event B must give thread B the abandoned mutex (`WAIT_ABANDONED_0`). Needs
`/dev/ntsync` in use.

| | `waitall` |
|---|---|
| Stock, or patch 4 alone | never returns |
| Patches 4 and 5, 64- and 32-bit programs | B gets `WAIT_ABANDONED_0` as soon as its event is set: 30 of 30 per architecture on Experimental 20260924, 10 of 10 on 20261001 |
| Patches 4 to 6 of release `-4` on 20261014, 64- and 32-bit programs | B gets `WAIT_ABANDONED_0`, 3 of 3 per architecture |
| Patch 5 alone (patch 4's orphan list disabled), 64-bit | B gets `WAIT_ABANDONED_0`, 3 of 3 |
| Windows 11 25H2 | the same, 3 of 3 |

`kernel32:sync` and `ntdll:sync`: 0 failures with patches 1 to 5 on both bases; the server's descriptor count is
unchanged after 80 iterations of all three probe modes. With release `-4`'s commits on 20261014, ntsync on:
`kernel32:sync` fails 3 checks per run on stock (`test_mutex`'s two and patch 6's), 3 of 3 runs, and none with
the patches, in 5 runs of the 64-bit test and 3 of the 32-bit one; without ntsync, none either way. With patch 4
alone only patch 6's check fails; with patch 5 alone, nothing does, `test_mutex` included. A persistent
`wineserver`'s descriptor count stayed at 712 over 60 probe processes and one process doing 20 wait-all
iterations, each of which goes through a keep and a release. On Windows 11 patch 6 passes, 64- and 32-bit,
3 of 3 runs each ([`results/windows-wine-tests-2026-10-28.txt`](../results/windows-wine-tests-2026-10-28.txt)). The 32-bit runs matter here: under Proton a 32-bit
process uses the 32-bit unix `ntdll.so`, which must speak the new protocol.

`inproc_cache_probe.exe 5000` ([`repro/`](../repro)) creates 5000 events and waits on each one once, so the
cache has to map a second block of entries. With the 65520-byte block the process aborts at the wait on the
handle with index 3276 (the assertion above on stderr) and can then hang instead of exiting; with whole 64K
blocks it prints `waited on all 5000 events: OK`. Needs `/dev/ntsync` in use, since the cache is only used for
in-process synchronization.

## Upstream

Reported together with patch 4 in Wine bug 60417 (2026-10-01) and posted on Proton
[#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943056744) with the server diff; on
2026-10-17 it got its own report, Wine bug [60524](https://bugs.winehq.org/show_bug.cgi?id=60524), which links these commits. No reply on either as of
2026-10-28.
Written with an LLM; a reference for upstream, not a merge request.

What Windows does, for the record: the NT kernel keeps the mutexes a thread owns on a list in the thread itself,
filled when a wait acquires a mutant, under the dispatcher lock, and abandons that list when the thread is run
down; a wait holds a reference to the object, not to the handle, so closing the handle changes nothing (ReactOS,
which implements the same design: `KiSatisfyMutantWait` in `ntoskrnl/include/internal/ke_x.h`,
`KeRundownThread` in `ntoskrnl/ke/thrdobj.c`, `NtWaitForMultipleObjects` in `ntoskrnl/ob/obwait.c`). With
ntsync the acquisition happens in the kernel driver, which keeps no per-owner list: owner death is left to
userspace, one `NTSYNC_IOC_MUTEX_KILL` per mutex, so `wineserver` has to know every mutex a dying thread might
own, and patches 4 and 5 are the bookkeeping for that. The faithful fix would be in the ntsync driver, a
per-owner list or a device-level kill of everything a thread owns. Whether to propose that is an open question;
it is noted here and not pursued yet.
