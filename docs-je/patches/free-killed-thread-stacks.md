# Patches 7 and 8: free the stacks of a thread killed by another thread

| | |
|---|---|
| Commits | [`free-killed-thread-stacks`](https://github.com/jegglest/wine/commit/free-killed-thread-stacks) `ntdll: Free the TEB and stacks of a thread killed by another thread.` ([patch file](https://github.com/jegglest/wine/commit/free-killed-thread-stacks.patch)); [`free-killed-thread-stacks-test`](https://github.com/jegglest/wine/commit/free-killed-thread-stacks-test) `kernel32/tests: Test that the stack of a killed thread is freed.` ([patch file](https://github.com/jegglest/wine/commit/free-killed-thread-stacks-test.patch)); the tags move to the rebased commits with every new Proton Experimental |
| Changes | patch 7: `dlls/ntdll/unix/thread.c` and `dlls/ntdll/unix/unix_private.h`, 38 lines added, none removed. Patch 8: `dlls/kernel32/tests/thread.c`, 100 lines added: `test_TerminateThread_stack()` in `kernel32:thread` |
| Fixes | a thread killed with `TerminateThread()` keeps its TEB, its stack and its kernel stack, about 2 MiB of address space and whatever pages it touched, for as long as the process runs |
| Needs | nothing; independent of patches 2 to 6 |
| Applies to | Valve's Wine [`7b3ceaf8`](https://github.com/ValveSoftware/wine/commit/7b3ceaf8644bb5d57140903e12897efac69be16a) of Experimental 20261027b, both commits with `git am`, with or without patches 2 to 6 (checked 2026-11-01). GE-Proton11-11 (Valve's `c53678ab22a` with GE's patches), both, as [Proton-JE-GE](https://github.com/proton-je/proton-je-ge)'s patch files; only the context of patch 7's `unix_private.h` hunk differs. Wine master (`2528aff2620`, Wine 11.21): patch 8 with `git am`; patch 7 needs a port, since master keeps the per-thread data in a `struct thread_data` outside the TEB and frees it with `virtual_free_thread_data()`; the same change applies there |
| Upstream | not reported yet |
| Windows | frees them: "The system frees the thread's initial stack" ([`TerminateThread()`](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminatethread); only Windows XP and Server 2003 leaked it). Measured on Windows 11 25H2: 500 killed threads leave 1 or 2 allocations behind, 64- and 32-bit; patch 8 passes there, 64- and 32-bit |
| Full story | [Killed threads keep their stacks](../killed-thread-stacks.md) |

## The bug

A Wine thread ends in one of two places on the Unix side. One that returns or calls `ExitThread()` goes through
`exit_thread()`, which hands its TEB on to the next thread that exits: that thread joins it with `pthread_join()` and
frees its TEB, its stack and its kernel stack with `virtual_free_teb()`. A thread killed by another thread,
with `TerminateThread()`, never runs `exit_thread()`. The server signals it, or ends its wait, and it leaves
through `abort_thread()`, which only closes its descriptors and calls `pthread_exit()`. Its TEB, its stack and its
kernel stack stay mapped for the life of the process, and its TEB stays on the list of TEBs that
`virtual_set_tls_information()` and `virtual_clear_tls_index()` walk. A 64-bit thread with a 1 MiB stack keeps two
allocations, 2 MiB of address space; a 32-bit thread keeps three, its 64-bit stack included.

## The change

A killed thread cannot free its own stacks, since it is still running on its kernel stack, and `abort_thread()`
may run with a lock held: it is also where a thread ends when the server connection breaks in the middle of a
request, possibly inside `virtual_mutex` or `fd_cache_mutex`. So `abort_thread()` frees nothing. It pushes the
thread's TEB onto a lock-free list, through a new `next_killed` field at the end of `struct ntdll_thread_data`,
and leaves. `free_killed_threads()` takes the whole list with one atomic exchange, joins each thread and frees it
with `virtual_free_teb()`, as `exit_thread()` does with the thread before it. Two places call it, both outside any
lock: `exit_thread()`, before its own handoff, and `NtCreateThreadEx()`, before it allocates the new thread's TEB,
so that a program that kills threads but rarely ends one normally gets them back at its next thread creation.
The main thread, whose `pthread_id` Wine never records, is not queued, as `exit_thread()` already skips it.

Between a kill and the next creation or exit the thread's stacks are still mapped, a few threads at most, where
Windows frees them before the thread is signaled. A thread is only joined once it has queued itself, right before
`pthread_exit()`, so a join never waits for more than the end of that call.

Patch 8 is the conformance test, `test_TerminateThread_stack()` in `kernel32:thread`. It reads the stack of two
threads from their TEBs, `DeallocationStack` through `ThreadBasicInformation`: one killed while it sleeps, one
created suspended and killed before it ever ran, which is Helldivers 2's case. After each kill it runs short
threads until the stack is gone, `MEM_FREE` or part of another allocation, or in use as a new thread's stack, for
at most 50 rounds. Wine only knows a new thread's TEB once the thread has started up, so the test polls
`ThreadBasicInformation` until it has one.

## How to check it

`kernel32_test.exe thread` from the patched tree, and [`killed_thread_stacks.exe`](../repro/killed_thread_stacks.c)
([`repro/`](../repro)), which counts what 500 threads leave in the address space: killed while sleeping (`kill`),
created suspended with no start address and killed without running, every other one hidden from debuggers
(`suspended`, the game's pattern), or returning (`exit`).

| | `kernel32:thread` | `kill`, 500 threads | `suspended`, 500 threads | `exit`, 500 threads |
|---|---|---|---|---|
| Release `proton-je-11.0-20261027b-1`, 64-bit | 2 failures, both of patch 8 | +1016 allocations, +1530 MiB committed, +48 MiB working set | +1016, +1529 MiB, +10 MiB | none |
| The same, 32-bit | 2 failures, both of patch 8 | +1521, +2028 MiB, +48 MiB | +1510, +2019 MiB, +12 MiB | none |
| With patches 7 and 8, 64-bit | 0 failures | +1, +0.6 MiB, +0.5 MiB | +14, +21 MiB, +0.2 MiB | none |
| With patches 7 and 8, 32-bit | 0 failures | +1, +0.2 MiB, +0.4 MiB | +33, +45 MiB, +0.3 MiB | none |
| Windows 11 25H2, 64-bit | 0 failures | +1, +0.7 MiB, +0.7 MiB | none | none |
| Windows 11 25H2, 32-bit | patch 8 passes; 21 failures in the 32-bit-only `GetThreadContext()` and `GetThreadSelectorEntry()` tests, as without it | +2, +0.5 MiB, +0.6 MiB | none | none |

With the patches the `suspended` rows keep the last few threads killed, which are not yet on the list when the
program runs its last thread. GE-Proton11-11 with Proton-JE-GE's patch files behaves the same: `kernel32:thread`
fails the same 2 checks before and none after, 64- and 32-bit, and the same programs measure the same leak before
and none after. `ntdll:sync`, `kernel32:sync` and `ntdll:thread` pass with the patches, 64- and 32-bit, and with
Proton-JE-GE's build so do Proton-JE's other probes. Raw Windows output: [`results/windows-killed-thread-stacks-2026-11-01.txt`](../results/windows-killed-thread-stacks-2026-11-01.txt).
