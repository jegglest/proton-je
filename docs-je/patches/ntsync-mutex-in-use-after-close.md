# Patch 5: keep an in-process mutex that a process closed while still using it

| | |
|---|---|
| Commit | [`7dda1b8569`](https://github.com/jegglest/wine/commit/7dda1b8569e7658b85c1d9c5b18a0b7566f3bf8e) `server: Keep an in-process mutex that a process closed while still using it.` ([patch file](https://github.com/jegglest/wine/commit/7dda1b8569e7658b85c1d9c5b18a0b7566f3bf8e.patch)) |
| Changes | 8 files, 186 lines added and 18 removed: `server/inproc_sync.c`, `server/handle.c`, `server/process.c`, `server/object.h`, `server/protocol.def` (a field added to `close_handle` and `dup_handle` and one new request, `release_inproc_sync`, so the server protocol version changes), `dlls/ntdll/unix/sync.c`, `dlls/ntdll/unix/server.c`, `dlls/ntdll/unix/unix_private.h` |
| Fixes | the case patch 4 leaves open: a mutex that is unowned when its last handle is closed, with a wait-all still pending on it, can be acquired later; if that thread then dies the mutex is again not abandoned and a second waiter blocks forever |
| Needs | [patch 4](ntsync-mutex-owned-after-close.md). Because of the protocol change, `wineserver` and both unix-side `ntdll.so` (64- and 32-bit) must come from the same tree: automatic in a full build, and the reason a build that only replaces files must replace all three (Space Marine 2 starts through a 32-bit launcher stub on every launch) |
| Applies to | with patch 4: Valve's Wine [`6d211aab`](https://github.com/ValveSoftware/wine/commit/6d211aabd1d325c9990a5fd7a81b9db37caae12d) and wine-cachyos as of 2026-10-04 with `git am`, no conflicts. Wine master as of 2026-10-09: hand port needed, the hunks in `server/inproc_sync.c` and `dlls/ntdll/unix/sync.c` collide with Proton's fsync fallback next to the changed lines |
| Upstream | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417), the second part of the report; UNCONFIRMED |
| Windows | abandons it: the second wait-all waiter gets `WAIT_ABANDONED_0` as soon as its event is set, 3 of 3 |
| Full story | [ntsync mutexes are never abandoned once their handles are closed](../ntsync-mutex-abandonment.md) |

## The bug

Patch 4 keeps a mutex that is owned at the moment its server object is destroyed. A mutex that is unowned at
that moment is forgotten just the same, and a wait that was already pending on it, for instance a wait-all
whose other objects are signaled later, can still acquire it through the duplicate descriptor it holds. If that
thread then dies, nothing sends the kill, the mutex stays owned by a dead thread id, and another pending waiter
blocks forever. Windows abandons it there too.

## The change

The client-side cache entry of an in-process sync counts its users, so `NtClose()` (and `NtDuplicateObject()`
with `DUPLICATE_CLOSE_SOURCE`) can tell when another thread of the process is still using the mutex. It then
passes the entry's serial number with the `close_handle` or `dup_handle` request; the server keeps a reference
to the in-process sync under that serial, which keeps the mutex on the list it kills at thread death. The
entry's last user drops it with the new `release_inproc_sync` request, and a process's records go away with the
process. A handle closed with no other user, the common case, involves no extra work. This is the design the
comment above `struct inproc_sync` in `dlls/ntdll/unix/sync.c` already describes, done with a serial number
instead of a handle. The new request is appended after the existing ones, so request numbers do not change.

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

## How to check it

`mutex_abandon.exe waitall` ([`repro/`](../repro)): the mutex is unowned when its handle is closed and two
threads are blocked in wait-all calls on it, each with its own unset event. Setting event A lets thread A take
the mutex; A is killed; setting event B must give thread B the abandoned mutex (`WAIT_ABANDONED_0`). Needs
`/dev/ntsync` in use.

| | `waitall` |
|---|---|
| Stock, or patch 4 alone | never returns |
| Patches 4 and 5, 64- and 32-bit programs | B gets `WAIT_ABANDONED_0` as soon as its event is set: 30 of 30 per architecture on Experimental 20260924, 10 of 10 on 20261001 |
| Windows 11 25H2 | the same, 3 of 3 |

`kernel32:sync` and `ntdll:sync`: 0 failures with patches 1 to 5 on both bases; the server's descriptor count is
unchanged after 80 iterations of all three probe modes. The 32-bit runs matter here: under Proton a 32-bit
process uses the 32-bit unix `ntdll.so`, which must speak the new protocol.

`inproc_cache_probe.exe 5000` ([`repro/`](../repro)) creates 5000 events and waits on each one once, so the
cache has to map a second block of entries. With the 65520-byte block the process aborts at the wait on the
handle with index 3276 (the assertion above on stderr) and can then hang instead of exiting; with whole 64K
blocks it prints `waited on all 5000 events: OK`. Needs `/dev/ntsync` in use, since the cache is only used for
in-process synchronization.

## Upstream

Reported together with patch 4 in Wine bug 60417 (2026-10-01, no reply as of 2026-10-09) and posted on Proton
[#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5943056744) with the server diff.
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
