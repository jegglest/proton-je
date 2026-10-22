# Helldivers 2: a critical section in a stack frame its renderer has already left

| | |
|---|---|
| Commit | commit 32 on [`jegglest/wine`, branch `je-11.0`](https://github.com/jegglest/wine/commits/je-11.0), tag [`helldivers2-cs-wake-guard`](https://github.com/jegglest/wine/commit/helldivers2-cs-wake-guard) (`dlls/ntdll/sync.c`, `dlls/ntdll/loader.c`) |
| Kind | a game workaround, not a Wine fix: a `HACK:` commit in the form Valve's Wine uses for them, on for `helldivers2.exe` only; `WINE_CS_WAKE_GUARD=0` turns it off, `=1` turns it on for any game |
| Seen as | Helldivers 2 closing through its crash reporter since the game update of 2026-10-21 (Steam build 25988413, `game.dll` 1.0.0.19374, `helldivers2.exe` 1.8.46539.0), and still after the next one (the crash reporter's build tag 19411) |
| Affects | every Proton tried: Proton Experimental 11.0-20261014 and `proton-je-11.0-20261014-1` crash the same way |
| Without this build | nothing equivalent: Valve's `WINE_ALERT_SIMULATE_SCHED_QUANTUM=1`, tried first, does nothing on Linux 7.2 and later ([why](#valves-switch-does-nothing-on-linux-72)) |
| Windows | not known: no reports of this crash from Windows players found on 2026-10-21 |
| Upstream | not sent; Valve's Wine takes per-game workarounds as pull requests, and the bug itself is Arrowhead's |

## Summary

The game's renderer thread builds an object holding a critical section on its stack, hands it to the game's
`D3D12TaskDispatcherThread` and waits on a flag with `WaitOnAddress()`. When the dispatcher wakes it while still
inside the critical section, and the renderer runs before the dispatcher is out of `LeaveCriticalSection()`, the
renderer returns and reuses the frame, and the dispatcher finishes the leave on whatever is there now. In the
crashes, what is there reads as a waiting thread, so the leave goes on to release `LockSemaphore`, which now holds
a value that is not a handle: `ntdll` raises a non-continuable `STATUS_INVALID_HANDLE`, the game's crash reporter
catches it and the game closes. The race is the game's. The commit closes the window the crashes go through: a
thread woken from `RtlWaitOnAddress()` by a thread that holds a critical section on the woken thread's stack does
not return until the waker is out of `RtlLeaveCriticalSection()` on it.

## Symptom

The game writes a minidump per crash to `crash_data/reports/` and `dumps/` under
`steamapps/compatdata/553850/pfx/drive_c/users/steamuser/AppData/Roaming/Arrowhead/Helldivers2/`. Six of them,
from consecutive sessions after the update, are the same, and so are three more from sessions after the game's
next update:

- exception `0xC0000008` (`STATUS_INVALID_HANDLE`), flags `EXCEPTION_NONCONTINUABLE`, no parameters: the record
  `RtlRaiseStatus()` builds;
- on the thread named `D3D12TaskDispatcherThread::thread`;
- stack, with the symbols of the Proton build's `ntdll.dll`, whose time stamp matches the module in the dump:
  `ntdll!raise_status+0x48` ← `RtlRaiseStatus+0xf` ← `RtlpUnWaitCriticalSection+0x4f` ←
  `RtlLeaveCriticalSection+0x9b` ← `helldivers2.exe+0x7086fe`;
- nothing in the kernel log: no GPU reset, no page fault, no OOM kill;
- the same with Proton Experimental and with proton-je, and with the graphics settings lowered.

The crash reporter logs `exception captured`, uploads the first report and rate-limits the next ones. An older
dump in the same directory, from before the update, is unrelated: an access violation at `helldivers2.exe+0x623cc6`.

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
take a value above 1 for a semaphore handle, which they release with `NtReleaseSemaphore()` and raise on failure
with `RtlRaiseStatus()` (`dlls/ntdll/sync.c`, since Wine commit `2745228b` of 2024). `RtlLeaveCriticalSection()`
decrements `RecursionCount`; at zero it clears `OwningThread` and decrements `LockCount`, and if `LockCount` is
still 0 or more a thread is waiting, so it calls `RtlpUnWaitCriticalSection()`.

On a reused frame those fields hold the renderer's new data. Under Wine the game's image sits at its preferred
base `0x140000000`, and the renderer's stack was between 4 and 8 GB in five of the six dumps, so a pointer stored
over `LockCount` and `RecursionCount` reads as `RecursionCount` 1 and a large `LockCount`: the leave believes it is
the last level and that a thread is waiting, and releases `LockSemaphore`, which holds another such value. That
is the likely reading, not an observation: the dumps show the outcome, not the values, because the renderer had
initialized the next critical section over them before the dump was written.

Nothing in Wine's critical section code is wrong here, and nothing in it can tell a stale frame from a live lock.

## What Windows does

Windows takes `LockSemaphore` for a handle as well, and raises the same exception when it is not one: Wine's
`kernel32:sync` test from commit `bf7d403a` (Wine bug [57011](https://bugs.winehq.org/show_bug.cgi?id=57011)) sets
`LockSemaphore` to `0xdeadbeef` and checks that `EnterCriticalSection()` raises a non-continuable
`STATUS_INVALID_HANDLE`, and it passes on Windows. So raising is not the difference. What differs is how often the
race is lost, and possibly what the stale frame holds: with Windows' address-space layout a stale pointer would
rarely read as `RecursionCount` 1. Whether the game crashes on Windows is not known; it was not measured here.

## The fix

### Valve's switch does nothing on Linux 7.2

Valve's Wine has a switch for races where a woken thread runs before its waker has finished:
`alert_simulate_sched_quantum` in `dlls/ntdll/unix/loader.c`, added by Paul Gofman in 2022 for GTA V
(`34e74948`) and since turned on for Mary Skelter 2 (`57eaadb8`) and six more Idea Factory games (`c68af942`,
[ValveSoftware/wine#274](https://github.com/ValveSoftware/wine/pull/274)). When it is on, a thread that blocked in
`NtWaitForAlertByThreadId()` calls `usleep(0)` once it is woken (`dlls/ntdll/unix/sync.c`). That used to sleep
for the thread's timer slack, 50 µs by default, and so let the waker run on. It was the first thing tried here:
three sessions with `WINE_ALERT_SIMULATE_SCHED_QUANTUM=1` on `proton-je-11.0-20261014-1` crashed the same way.

Since Linux 7.2 a zero-length sleep no longer sleeps. Commit
[`b40c927345a9`](https://git.kernel.org/torvalds/c/b40c927345a9) ("hrtimer: Use hrtimer_start_expires_user() for
hrtimer sleepers") checks a sleeper's requested expiry as the timer is queued and returns at once if it has
already passed, so the slack never applies. On Linux 7.2.9, `usleep(0)` and a bare `nanosleep({0, 0})` system call
return in about 0.4 µs, while `usleep(1)` still sleeps about 52 µs. The switch is just as inert for the games
Valve turns it on for, and so is `WINE_FSYNC_SIMULATE_SCHED_QUANTUM`, which also calls `usleep(0)`. Where it does
sleep, it only narrows the window.

### Why the race is lost so often

The dispatcher's side of the window is a few instructions long: from the return of the futex system call that
wakes the renderer to the decrement of `LockCount` in `RtlLeaveCriticalSection()`. The renderer needs several
microseconds to come back from its wait, return and reuse the frame, so it can only win when the dispatcher loses
its CPU inside that window. The likely way, not measured here, is the wake itself: when no CPU is idle, Linux
can queue the woken renderer on the dispatcher's CPU and preempt the dispatcher as it returns from the futex
system call, exactly inside the window. The reproducer below cannot lose the race on an idle machine without a
stall standing in for that preemption.

### What the commit does

With `WINE_CS_WAKE_GUARD=1`, or by default for `helldivers2.exe`:

- Each thread keeps a list of up to 14 critical sections it holds, in the TEB's
  `ReservedForDebuggerInstrumentation`, which Wine does not otherwise use. `RtlEnterCriticalSection()` and
  `RtlTryEnterCriticalSection()` add a critical section when they take it, and the last
  `RtlLeaveCriticalSection()` removes it.
- `RtlWaitOnAddress()` records the top of the waiter's stack in its queue entry. When `RtlWakeAddressSingle()` or
  `RtlWakeAddressAll()` dequeues a waiter, it looks for a critical section that the waker owns between that
  entry and the top of the waiter's stack, and notes it in the entry before it releases the entry.
- The woken waiter, before `RtlWaitOnAddress()` returns, waits until the waker no longer owns that critical section
  and is out of `RtlLeaveCriticalSection()`, which marks the critical section it is releasing before it clears
  `OwningThread` and unmarks it after its last access. It spins briefly, then yields, then sleeps in 100 µs steps,
  and gives up after about 30 ms with one `still held by` error in the log, so a waker that keeps the lock cannot
  hang it.
- Waits for a critical section itself are not guarded: such a waiter would otherwise wait for the very lock it
  is about to take.

`ntdll` logs `HACK: Enabling critical section wake guard.` at startup (debug channel `module`), and the give-up
error goes to channel `sync`. `WINEDEBUG=-all,err+module,err+sync` in the launch options shows both without
`PROTON_LOG`. The cost, when on, is about 0.6 ns per uncontended `EnterCriticalSection()`/`LeaveCriticalSection()`
pair (9.4 against 8.85 ns); wakes that involve no such critical section are unchanged, and nothing changes when it
is off.

A renderer that finds the flag set without having blocked is not covered; that race does not depend on the wake.
The fix for good is the game's: the renderer must not return while the dispatcher can still touch the object, so
the dispatcher has to be done with it before it lets the renderer go.

## Testing

### Reproducer

[`repro/stack_cs_handoff.c`](repro/stack_cs_handoff.c) does the game's handoff: the main thread puts an object
holding a critical section and a flag on its stack and waits for the flag with `WaitOnAddress()`; another thread
enters the critical section, sets the flag, wakes it, stalls for a given time to hold the window open, and leaves.
It counts the handoffs where the waiter returned before the leave had finished. Run with the Wine of
`proton-je-11.0-20261014-1` and this commit's `ntdll.dll`, Linux 7.2.9:

| Mode | Stall | `WINE_CS_WAKE_GUARD=0` | `WINE_CS_WAKE_GUARD=1` |
|---|---|---|---|
| critical section on the waiter's stack | 200 µs | 200 of 200 returned early | 0 of 200 |
| critical section on the waiter's stack | none | 0 of 10000 | 0 of 5000 |
| critical section on the waiter's stack | 5 ms | | 0 of 100 |
| critical section on the waiter's stack | 100 ms | | 5 of 5, each after about 30 ms, one error logged |
| object on the heap | 200 µs | 200 of 200 | 200 of 200: the guard is not involved |
| `SleepConditionVariableCS()`, with a third thread contending | 200 µs | 0 of 200 | 0 of 200, no added delay |

Wine's `ntdll:sync` (441 tests) and `kernel32:sync` (about 470,000) pass with 0 failures with the guard on and off,
as with `proton-je-11.0-20261014-1`. The cost above was measured with
[`repro/cs_enter_leave_bench.c`](repro/cs_enter_leave_bench.c).

### In the game

Pending, with the build that carries the commit. Without it, every session since the update has ended in this
crash, including the three with `WINE_ALERT_SIMULATE_SCHED_QUANTUM=1`.

### Recognizing the crash in a dump

A dump shows this crash when its exception is `0xC0000008` with no parameters, on the thread named
`D3D12TaskDispatcherThread::thread` (the thread-names stream, type 24), and the critical section that
`raise_status()` saved, at `rsp + 0xc8` of the exception context in the Proton Experimental and proton-je builds of
11.0-20261014, lies inside the stack of the thread named `renderer`. The symbols come from the matching Proton
build's `files/lib/wine/x86_64-windows/ntdll.dll`, which keeps its COFF symbol table, so `objdump -d` names the
internal functions too.

## Upstream status

Not sent. Valve's Wine takes per-game workarounds as pull requests, as for the Idea Factory games in
ValveSoftware/wine#274; this one is new code rather than an entry in an existing list. That Linux 7.2 turned
Valve's sched-quantum switches into no-ops is worth a report to Valve on its own. The bug itself is the game's,
for Arrowhead to fix; their crash reporter has the reports (it uploads the first one and rate-limits the rest).

## References

- Wine commit [`2745228b`](https://gitlab.winehq.org/wine/wine/-/commit/2745228b14d), "ntdll: Don't use debug info
  presence to detect critical section global status.": `LockSemaphore` as a futex word, a handle only above 1.
- Wine commit [`bf7d403a`](https://gitlab.winehq.org/wine/wine/-/commit/bf7d403ad89), "ntdll: Raise exception on
  failed CS wait." (Wine bug [57011](https://bugs.winehq.org/show_bug.cgi?id=57011)), with the `kernel32:sync` test
  that shows what Windows does with an invalid `LockSemaphore`.
- Valve's Wine commits `34e74948` ("ntdll: HACK: Add WINE_ALERT_SIMULATE_SCHED_QUANTUM option."), `57eaadb8` (Mary
  Skelter 2) and `c68af942` (six more games, [ValveSoftware/wine#274](https://github.com/ValveSoftware/wine/pull/274)).
- Linux commit [`b40c927345a9`](https://git.kernel.org/torvalds/c/b40c927345a9), "hrtimer: Use
  hrtimer_start_expires_user() for hrtimer sleepers" (v7.2): already-expired sleeps return without sleeping.
