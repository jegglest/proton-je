# Helldivers 2: a critical section in a stack frame its renderer has already left

| | |
|---|---|
| Commit | commit 32 on [`jegglest/wine`, branch `je-11.0`](https://github.com/jegglest/wine/commits/je-11.0), tag [`cs-unwait-ignore-errors`](https://github.com/jegglest/wine/commit/cs-unwait-ignore-errors) (`dlls/ntdll/sync.c`, seven lines): Paul Gofman's "ntdll: Ignore semaphore errors in RtlpUnWaitCriticalSection()" from Valve's Wine |
| Kind | a fix from Valve's newer Wine, taken from its bleeding-edge branch before it reaches Proton Experimental; on for every game, nothing to switch |
| Seen as | Helldivers 2 closing through its crash reporter since the game update of 2026-10-21 (Steam build 25988413, `game.dll` 1.0.0.19374, `helldivers2.exe` 1.8.46539.0), and still after the next one (the crash reporter's build tag 19411) |
| Affects | every Proton tried here: Proton Experimental 11.0-20261014 and `proton-je-11.0-20261014-1`; other players report the same crash in Proton issue [#7486](https://github.com/ValveSoftware/Proton/issues/7486), with Proton 11.0-2 too, less often |
| Without this build | Proton Experimental's [bleeding-edge branch](https://github.com/ValveSoftware/Proton/wiki/Proton-Versions#proton-bleeding-edge) has carried the same commit since 2026-10-20 |
| Releases | `proton-je-11.0-20261014-3` and later. `-2` carried a workaround of this repository's own instead ([why it was replaced](#what-release--2-carried-instead)) |
| Windows | Windows raises for a bad `LockSemaphore` when `EnterCriticalSection()` has to wait; whether it raises in the leave, where the game dies, is what the probe checks; its Windows run is pending ([details](#what-windows-does)) |
| Upstream | Valve's change (CodeWeavers bug 27891), in bleeding-edge, not yet in Proton Experimental; upstream Wine still raises there |

## Summary

The game's renderer thread builds an object holding a critical section on its stack, hands it to the game's
`D3D12TaskDispatcherThread` and waits on a flag with `WaitOnAddress()`. The dispatcher sets the flag and wakes the
renderer while it is still inside the critical section. When the renderer runs before the dispatcher is out of
`LeaveCriticalSection()`, it returns and reuses the frame, and the dispatcher finishes the leave on whatever is
there now. Under Wine, what is there reads as a waiting thread and a semaphore handle, the release of that handle
fails, and `ntdll` raised a non-continuable `STATUS_INVALID_HANDLE` for it; the game's crash reporter caught it and
the game closed. Valve's fix, which this build carries, makes `RtlpUnWaitCriticalSection()` ignore a
failed release instead of raising, so the late leave returns and the game carries on.

## Symptom

The game writes a minidump per crash to `crash_data/reports/` and `dumps/` under
`steamapps/compatdata/553850/pfx/drive_c/users/steamuser/AppData/Roaming/Arrowhead/Helldivers2/`. Every one of
them since the update, on two machines and after the game's next update too, is the same:

- exception `0xC0000008` (`STATUS_INVALID_HANDLE`), flags `EXCEPTION_NONCONTINUABLE`, no parameters: the record
  `RtlRaiseStatus()` builds;
- on the thread named `D3D12TaskDispatcherThread::thread`;
- stack, with the symbols of the Proton build's `ntdll.dll`, whose time stamp matches the module in the dump:
  `ntdll!raise_status+0x48` ← `RtlRaiseStatus+0xf` ← `RtlpUnWaitCriticalSection+0x4f` ←
  `RtlLeaveCriticalSection+0x9b` ← `helldivers2.exe+0x7086fe`;
- nothing in the kernel log: no GPU reset, no page fault, no OOM kill;
- the same with Proton Experimental and with proton-je, and with the graphics settings lowered.

The crash reporter logs `exception captured`, uploads the first report and rate-limits the next ones. The game's
anti-tamper writes a dump of its own, `THEIA_*.dmp` in the prefix's `AppData/Local/Temp`, which records a later
fault instead: a null read at `helldivers2.exe+0x9f60ed`, inside the game's exception handler, after the
`STATUS_INVALID_HANDLE`, as players' logs in #7486 show. An older dump from before the update is unrelated: an
access violation at `helldivers2.exe+0x623cc6`.

## The game side

`helldivers2.exe` is encrypted on disk, so the game's code cannot be read statically, and the dumps hold only the
threads' stacks. That is enough:

- `RtlLeaveCriticalSection()` keeps the critical section's address in `rbx`, which `RtlpUnWaitCriticalSection()`
  and `RtlRaiseStatus()` leave alone and `raise_status()` saves on its stack: that gives the critical section in
  every dump.
- It lies on the stack of the thread named `renderer`, 0x5b0 bytes above where that thread is blocked, always at
  the same place in its frame (the addresses end in `98b8`). At `+0x28` from it is the same `.rdata` pointer in
  every dump, `helldivers2.exe+0x1735448`, presumably a vtable: the critical section is a member of a C++ object
  on the renderer's stack.
- The dispatcher's stack holds the address of that critical section, and of nothing else on the renderer's stack.
- At the time of the dump the renderer is in `kernelbase!WaitOnAddress()` → `RtlWaitOnAddress()` →
  `NtWaitForAlertByThreadId()`, waiting on an address 0x2bc above the critical section.
- The critical section there is a fresh one: `DebugInfo` -1 (no debug info, the default since Windows 8),
  `LockCount` -1, `RecursionCount` 0, `OwningThread` 0, `LockSemaphore` 0, `SpinCount` 4000. It is not the one the
  dispatcher was leaving, which had a waiter and a `LockSemaphore` above 1: by the time of the dump the renderer
  had returned, built the next object in the same place and was waiting on it, for a dispatcher that was no
  longer running.

So the renderer can be released, and leave the frame, while the dispatcher is still inside
`LeaveCriticalSection()` on the object in it.

## The Wine side

`ntdll`'s critical sections use `LockSemaphore` as a futex word, 0 or 1, waited on with `RtlWaitOnAddress()`, and
take a value above 1 for a semaphore handle, which they release with `NtReleaseSemaphore()` (`dlls/ntdll/sync.c`,
since Wine commit `2745228b` of 2024). `RtlLeaveCriticalSection()` decrements `RecursionCount`; at zero it clears
`OwningThread` and decrements `LockCount`, and if `LockCount` is still 0 or more a thread is waiting, so it calls
`RtlpUnWaitCriticalSection()`.

On a reused frame those fields hold the renderer's new data. Under Wine the game's image sits at its preferred
base `0x140000000`, although it is built for high-entropy ASLR, and the renderer's stack was between 4 and 8 GB in
most dumps, so a pointer into either, stored over `LockCount` and `RecursionCount`, reads as `RecursionCount` 1 and
a large `LockCount`: the leave believes it is the last level and that a thread is waiting, and releases
`LockSemaphore`, which holds another such value. That is the likely reading, not an observation: the dumps show
the outcome, not the values, because the renderer had initialized the next critical section over them before the
dump was written. The probe below reproduces it with such values.

The release fails with `STATUS_INVALID_HANDLE`, and `RtlpUnWaitCriticalSection()` raised that status. That raise
is the step that turns the late leave into a crash, and it is old: it has been in Wine's critical section code
since before 2006. The raise on the other path, when `EnterCriticalSection()` has to wait on a bad
`LockSemaphore`, came in 2024 with a conformance test that checks it against Windows (Wine commit `bf7d403a`, Wine
bug [57011](https://bugs.winehq.org/show_bug.cgi?id=57011)). Nothing checks the leave.

### Why the race is lost so often

The dispatcher's side of the window is a few instructions long: from the return of the futex system call that
wakes the renderer to the decrement of `LockCount` in `RtlLeaveCriticalSection()`. The renderer needs several
microseconds to come back from its wait, return and reuse the frame, so it can only win when the dispatcher loses
its CPU inside that window. The likely way, not measured here, is the wake itself: when no CPU is idle, Linux can
queue the woken renderer on the dispatcher's CPU and preempt the dispatcher as it returns from the futex system
call, exactly inside the window.

## What Windows does

Windows takes `LockSemaphore` for a handle too, and raises a non-continuable `STATUS_INVALID_HANDLE` when
`EnterCriticalSection()` has to wait on one that is not valid: Wine's `kernel32:sync` test from commit `bf7d403a`
sets `LockSemaphore` to `0xdeadbeef` and checks exactly that, and it passes on Windows. That test covers the enter
path only. Whether Windows' `LeaveCriticalSection()` raises too when its release fails is what the probe below
checks; its Windows run is pending.

The layout differs in any case. Windows places a high-entropy-ASLR executable far above 8 GB, and its thread
stacks almost always above 8 GB too, so a stale pointer there reads as a large `RecursionCount`, and the leave
stops at decrementing it and `LockCount` without reaching the release; Wine's leave does the same with such values
(`stale-high` below). Whether the game
crashes on Windows is not known.

## The fix

Commit 32 is Valve's, Paul Gofman's "ntdll: Ignore semaphore errors in RtlpUnWaitCriticalSection()" (CodeWeavers
bug 27891), on Valve's bleeding-edge branch since 2026-10-20, taken here as it is. The leave still releases the
semaphore, or wakes the futex waiter, but no longer raises when the release fails:

```diff
 NTSTATUS WINAPI RtlpUnWaitCriticalSection( RTL_CRITICAL_SECTION *crit )
 {
-    NTSTATUS ret;
     HANDLE sem = get_semaphore( crit );

-    if (sem) ret = NtReleaseSemaphore( sem, 1, NULL );
+    if (sem) NtReleaseSemaphore( sem, 1, NULL );
     else
     {
         LONG *lock = (LONG *)&crit->LockSemaphore;
         InterlockedExchange( lock, 1 );
         RtlWakeAddressSingle( lock );
-        ret = STATUS_SUCCESS;
     }
-    if (ret) RtlRaiseStatus( ret );
-    return ret;
+    return STATUS_SUCCESS;
 }
```

It covers every way into the exception: a renderer woken by the dispatcher, one that finds the flag already set
without waiting, and any other code that leaves a critical section it no longer owns. It changes no timing and
needs no switch. The late leave still runs: it clears `OwningThread` and decrements `RecursionCount` and
`LockCount` in whatever the renderer has put in the frame since. Nothing on Windows stops a late leave either, so
what the game does with those writes is up to the game, as it would be there.

Once Valve's change reaches Proton Experimental, the rebase onto that Experimental drops commit 32, and the
`-<n>` releases on it get the fix from Valve's Wine itself.

### What release -2 carried instead

Release `-2` had a workaround of this repository's own, still reachable as tag
[`helldivers2-cs-wake-guard`](https://github.com/jegglest/wine/commit/helldivers2-cs-wake-guard) on the fork, with
its [write-up as
released](https://github.com/jegglest/proton-je/blob/proton-je-11.0-20261014-2/docs-je/helldivers2-renderer-race.md):
a thread woken from `RtlWaitOnAddress()` by a thread that held a critical section on the woken thread's stack
waited, for at most about 30 ms, until the waker was out of `RtlLeaveCriticalSection()`. That kept the late leave
out of the renderer's frame entirely, but `-3` replaces it with Valve's change because:

- it missed a renderer that finds its flag already set without waiting, and any wait past its 30 ms limit; Valve's
  change acts where the exception is raised, so it covers both;
- it was about 150 lines in `ntdll`'s critical section and wake paths, with per-thread bookkeeping in a spare TEB
  field and a per-game switch, against Valve's seven lines;
- it changed timing, and Valve's change does not;
- it would have had to be carried through every rebase, where Valve's change drops out once it reaches Proton
  Experimental;
- it rested on Windows raising the same exception, citing the 2024 conformance test, which covers the enter path
  only.

Valve's fix was in the bleeding-edge branch before that workaround was written, and #7486 had already pointed
players at that branch. Its reproducer and benchmark, `stack_cs_handoff.c` and `cs_enter_leave_bench.c`,
are in the `-2` tree.

### Valve's switch does nothing on Linux 7.2

Valve's Wine has a switch for races where a woken thread runs before its waker has finished:
`alert_simulate_sched_quantum` in `dlls/ntdll/unix/loader.c`, added by Paul Gofman in 2022 for GTA V
(`34e74948`) and since turned on for Mary Skelter 2 (`57eaadb8`) and six more Idea Factory games (`c68af942`,
[ValveSoftware/wine#274](https://github.com/ValveSoftware/wine/pull/274)). When it is on, a thread that blocked in
`NtWaitForAlertByThreadId()` calls `usleep(0)` once it is woken (`dlls/ntdll/unix/sync.c`). That used to sleep
for the thread's timer slack, 50 µs by default, and so let the waker run on. It was the first thing tried here,
and the game crashed the same way with it.

Since Linux 7.2 a zero-length sleep no longer sleeps. Commit
[`b40c927345a9`](https://git.kernel.org/torvalds/c/b40c927345a9) ("hrtimer: Use hrtimer_start_expires_user() for
hrtimer sleepers") checks a sleeper's requested expiry as the timer is queued and returns at once if it has
already passed, so the slack never applies. On Linux 7.2.9, `usleep(0)` and a bare `nanosleep({0, 0})` system call
return in about 0.4 µs, while `usleep(1)` still sleeps about 52 µs. The switch is just as inert for the games
Valve turns it on for, and so is `WINE_FSYNC_SIMULATE_SCHED_QUANTUM`, which also calls `usleep(0)`. Where it does
sleep, it only narrows the window.

## Testing

### The probe

[`repro/cs_leave_bad_semaphore.c`](repro/cs_leave_bad_semaphore.c) checks what `LeaveCriticalSection()` does when
it has a waiter to wake and `LockSemaphore` is not a valid handle, three ways, each in a process of its own:
`waiter` (a second thread blocked in `EnterCriticalSection()`, then `LockSemaphore` set to `0xdeadbeef`), `stale`
(the fields hold pointers between 4 and 8 GB, as a reused stack frame does under Wine) and `stale-high` (pointers
where Windows puts them, above 8 GB). Wine runs on Linux 7.2.9:

| Mode | What the leave sees | Proton Experimental 11.0-20261014 | `-2` | `-3` | Windows 11 |
|---|---|---|---|---|---|
| `waiter` | a thread blocked in `EnterCriticalSection()`, then `LockSemaphore` set to `0xdeadbeef` | raises `0xc0000008`, flags 1 | raises `0xc0000008`, flags 1 | returns | pending |
| `stale` | `RecursionCount` 1, `LockCount` `0x3ff998b8`, `LockSemaphore` `0x141735448` | raises `0xc0000008`, flags 1 | raises `0xc0000008`, flags 1 | returns | pending |
| `stale-high` | `RecursionCount` 168, `LockCount` `0x3c5ff8b8`, `LockSemaphore` `0x7ff6c1d20010` | returns | returns | returns | pending |

The `-3` column was measured before the release, with `-2`'s files and an `ntdll.dll` built from commit 32's
tree, the only file the commit changes; its exports and system call stubs match `-2`'s. Wine's `kernel32:sync`
test of a bad `LockSemaphore` covers `EnterCriticalSection()` only, which the commit does not touch.

### In the game

Pending with this release, on the machine where the game crashed every time since the update.

### Recognizing the crash in a dump

A dump shows this crash when its exception is `0xC0000008` with no parameters, on the thread named
`D3D12TaskDispatcherThread::thread` (the thread-names stream, type 24), and the critical section that
`raise_status()` saved, at `rsp + 0xc8` of the exception context in the Proton Experimental and proton-je builds of
11.0-20261014, lies inside the stack of the thread named `renderer`. The symbols come from the matching Proton
build's `files/lib/wine/x86_64-windows/ntdll.dll`, which keeps its COFF symbol table, so `objdump -d` names the
internal functions too. With this release the exception is gone, so a crash on that thread with another signature
is a different problem.

## Upstream status

Valve has the fix in its bleeding-edge branch; it reaches Proton Experimental with Valve's next update of that
branch. Upstream Wine still raises in `RtlpUnWaitCriticalSection()`; if the probe shows that Windows' leave does
not, that difference is worth a Wine bug, with the probe as its test. The race itself is in the game's code,
and Arrowhead's crash reporter has the reports (it uploads the first one and rate-limits the rest).

## References

- Valve's Wine, bleeding-edge branch: "ntdll: Ignore semaphore errors in RtlpUnWaitCriticalSection()", Paul Gofman,
  2026-10-20, CodeWeavers bug 27891; commit 32 here, tag
  [`cs-unwait-ignore-errors`](https://github.com/jegglest/wine/commit/cs-unwait-ignore-errors).
- Wine commit [`2745228b`](https://gitlab.winehq.org/wine/wine/-/commit/2745228b14d), "ntdll: Don't use debug info
  presence to detect critical section global status.": `LockSemaphore` as a futex word, a handle only above 1.
- Wine commit [`bf7d403a`](https://gitlab.winehq.org/wine/wine/-/commit/bf7d403ad89), "ntdll: Raise exception on
  failed CS wait." (Wine bug [57011](https://bugs.winehq.org/show_bug.cgi?id=57011)), with the `kernel32:sync` test
  that shows what Windows does with an invalid `LockSemaphore` in `EnterCriticalSection()`.
- Valve's Wine commits `34e74948` ("ntdll: HACK: Add WINE_ALERT_SIMULATE_SCHED_QUANTUM option."), `57eaadb8` (Mary
  Skelter 2) and `c68af942` (six more games, [ValveSoftware/wine#274](https://github.com/ValveSoftware/wine/pull/274)).
- Linux commit [`b40c927345a9`](https://git.kernel.org/torvalds/c/b40c927345a9), "hrtimer: Use
  hrtimer_start_expires_user() for hrtimer sleepers" (v7.2): already-expired sleeps return without sleeping.
- Proton issue [#7486](https://github.com/ValveSoftware/Proton/issues/7486), the game's compatibility report, where
  players report this crash.
