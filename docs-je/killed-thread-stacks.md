# Killed threads keep their stacks: Helldivers 2 grows by two stacks every 0.4 seconds

| | |
|---|---|
| Symptom | Helldivers 2 (Steam app 553850) maps more memory the longer it runs: about 4.8 thread stacks a second that are never freed, roughly 34 GiB of address space and 360 MiB of resident memory per hour |
| Cause | Wine never frees the TEB and the stacks of a thread killed with `TerminateThread()`; the game's anti-tamper code creates two suspended threads and kills them every 0.4 seconds |
| Windows | frees a killed thread's stack, as documented and as measured on Windows 11 |
| Fix | [patches 7 and 8](patches/free-killed-thread-stacks.md): the next thread that starts or exits frees a killed thread's TEB and stacks; plus a conformance test |
| In the game | with the patches the stacks no longer pile up ([below](#in-the-game)) |
| Upstream | not reported yet; proposed to GE-Proton as [#917](https://github.com/GloriousEggroll/proton-ge-custom/pull/917) |
| Found | 2026-11-01, in the game's own crash dumps |

## What the game does

The minidumps the game writes under Proton hold tens of thousands of private allocations of 1 MiB, in pairs: one
laid out like the stack Wine gives a thread (a reserved page, a guard page, the rest committed), one like the kernel
stack Wine gives it for system calls (a read-only guard page, the rest committed). Only a few dozen threads are
alive in each dump. Sampling `/proc/<pid>/task` of the running game every few microseconds shows where they come
from: threads that live 0.1 to 0.3 ms, born in pairs every 0.4 to 0.5 seconds, and named `wine_threadpool` when they
appear, which on Linux is the name of the thread that created them. Over 30 seconds the game's stack-shaped
mappings grew by 4.6 a second.

A run with `WINEDEBUG=+server` shows what they are. A worker of the Windows thread pool sends `new_thread` with flags
1, `THREAD_CREATE_FLAGS_CREATE_SUSPENDED`, or 5, the same plus `THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER`. The new
thread reports no start address, `init_thread(..., entry=00000000)`, and is told to stay suspended. The worker then
kills it with `terminate_thread` while it waits in its first `select`, before it runs any code:

```
0448: new_thread( process=ffffffff, access=001fffff, flags=00000005, request_fd=659, is_system=0, objattr={} )
0448: new_thread() = 0 { tid=04f4, handle=06c8 }
04f4: init_thread( unix_tid=66433, reply_fd=659, wait_fd=663, teb=2a83c0000, entry=00000000 )
04f4: init_thread() = 0 { suspend=1 }
04f4: select( flags=2, ... )
0448: terminate_thread( handle=06c8, exit_code=0 )
04f4: *killed* exit_code=0
```

In a 30-second run at the menu, 316 of the 432 threads the game created went this way, 158 hidden from debuggers
and 157 not, all killed by the worker that created them: 4.8 a second. The 26 workers that did it are the same
threads that make the game's anti-debugging calls in the same run (`NtSystemDebugControl()`,
`NtQuerySystemInformation(Ex)()` with classes 0x95 and 0xa3, `NtQueryInformationProcess()`,
`NtSetInformationThread()`). The check most likely compares how the system treats the two threads; it never needs
them to run.

## Why Wine keeps them

A thread that returns, or calls `ExitThread()`, ends in `exit_thread()` on the Unix side, which joins the thread
that ended before it and frees that thread's TEB, stack and kernel stack. A killed thread ends in
`abort_thread()` instead, which closes its descriptors and calls `pthread_exit()`, and nothing frees its memory
afterwards. Every killed thread of the game keeps two allocations, its 1 MiB stack and its 1 MiB kernel stack, and
the pages it touched: about 21 KiB for a thread that never ran. Its TEB also stays on the TEB list for the life of
the process.

The address space itself is not what runs out on 64-bit. What grows is resident memory, about 100 KiB a second, the
number of memory mappings, and the address space taken around the game's own allocations: once
the stacks are scattered over it, the game's 2 MiB and larger allocations no longer land 2 MiB-aligned and lose
the transparent huge pages Linux would give them, which makes freeing a 2 MiB allocation 39 to 43 µs instead of
3 µs, about what Windows takes for it (40 µs, measured in the same virtual machine).

## What Windows does

Microsoft's documentation of [`TerminateThread()`](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminatethread):
"The system frees the thread's initial stack. Windows Server 2003 and Windows XP: The target thread's initial
stack is not freed, causing a resource leak." Measured on Windows 11 25H2 with
[`killed_thread_stacks.exe`](repro/killed_thread_stacks.c), 500 threads each:

| | killed while sleeping | suspended, no start address, killed without running | returning |
|---|---|---|---|
| Windows 11 25H2, 64-bit | +1 allocation, +0.7 MiB committed | none | none |
| Windows 11 25H2, 32-bit | +2 allocations, +0.5 MiB | none | none |
| Proton-JE `proton-je-11.0-20261027b-1`, 64-bit | +1016 allocations, +1530 MiB | +1016, +1529 MiB | none |
| Proton-JE `proton-je-11.0-20261027b-1`, 32-bit | +1521 allocations, +2028 MiB | +1510, +2019 MiB | none |
| With patches 7 and 8, 64-bit | +1, +0.6 MiB | +14, +21 MiB, the last few threads killed | none |

The program is built with MinGW's default stack reservation of 2 MiB, where the game's is 1 MiB, so on Wine
each kill costs 3 MiB here: the stack and the 1 MiB kernel stack. A 32-bit thread under Wine also has a 64-bit
stack, three allocations in all. Raw output:
[`results/windows-killed-thread-stacks-2026-11-01.txt`](results/windows-killed-thread-stacks-2026-11-01.txt).

## The fix

[Patch 7](patches/free-killed-thread-stacks.md) has a killed thread put its TEB on a list in `abort_thread()`, which
may run with a lock held and so frees nothing, and has the next thread that starts or exits join it and free its
TEB and stacks. [Patch 8](patches/free-killed-thread-stacks.md) is the conformance test: a thread killed while it
waits and one killed before it ever ran must not leave their stacks allocated. It fails twice on stock Proton,
64- and 32-bit, and passes with the patch and on Windows 11. The page has the details and the measurements.

## In the game

Checked with Proton-JE-GE's release `proton-je-ge-11-11-2`, which carries patches 7 and 8 as patch files on
GE-Proton11-11, the same way as above: `/proc/<pid>/maps` and `/proc/<pid>/task` of the running game. The game
still creates and kills its threads, which a 30-second sample at 5 ms still catches as threads that come and go
between two samples, but their stacks no longer pile up. In those 30 seconds the
stack-shaped mappings grew by 1 user stack and 3 kernel stacks, where without the patches they grew by 137 and 144.
Sampled every 30 seconds for as long as the game ran, they stayed between 39 and 48 user stacks and 160 and 169
kernel stacks, for 137 to 145 live threads. The number of mappings went up and down between about 10,200 and 11,400
instead of growing by about 12 a second, and the resident memory rose from 3.5 to 5.3 GiB while the game loaded and
then stayed level.

## How it was found

Looking at a crash of the game at quit, which is something else: its anti-tamper code reads `game.dll` after the
game has unloaded it. Nothing here changes that crash. The dump of that crash had far more memory mapped than the
game uses, and nearly all of it was stack pairs of threads that no longer existed.
