# Stale thread alerts from `RtlWaitOnAddress()`

| | |
|---|---|
| Patches | commits 1 to 3 on [`jegglest/wine`, branch `je-11.0`](https://github.com/jegglest/wine/commits/je-11.0) (`dlls/ntdll/sync.c`, `dlls/ntdll/tests/sync.c`) |
| Patch pages | [patch 1](patches/waitonaddress-use-after-return.md) (the use-after-return) and [patches 2 and 3](patches/waitonaddress-stale-alert.md) (the stale alert, with the test): the commit, the patch file, what each needs and how to check it |
| Upstream | Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397) (ntdll), filed 2026-09-27, UNCONFIRMED |
| Seen as | Warhammer 40,000: Space Marine 2 crashing mid-mission under Proton: Proton issue [#8072](https://github.com/ValveSoftware/Proton/issues/8072) |
| Affects | every current Wine and Proton: the code in Proton Experimental's `ntdll` is identical to Wine master |
| Windows | does not have the problem, measured on Windows 11 with the programs in [`repro/`](repro) |

## Summary

`RtlWakeAddressSingle()` and `RtlWakeAddressAll()` take a waiter off the wait queue under a spinlock but alert
its thread only after releasing the lock. A waiter whose timeout expires in that window returns
`STATUS_TIMEOUT` and leaves the alert pending on its thread, and the thread's next wait on anything returns
immediately. The same alert mechanism is behind `SleepConditionVariableCS()`, `SleepConditionVariableSRW()`,
SRW locks and critical sections, so a program that polls `WaitOnAddress()` with a zero timeout (tcmalloc's
spinlock back-off does) collects stale alerts and then gets spurious wakeups from unrelated condition variables.
Windows consumes the racing wake instead and never leaves the alert behind. Space Marine 2 waits for its file
reads with a single unlooped `SleepConditionVariableCS(INFINITE)`, so a spurious wakeup there lets a read
callback write into a stack frame that no longer exists, and the game crashes.

The patches make the waiter take the in-flight alert, as Windows does (patch 2), fix a use-after-return in the
same code path (patch 1), and add a conformance test that fails on stock Wine and passes on Windows (patch 3).

## Symptom

The crash in #8072, reported by many people since September 2026, always has the same shape (game build
25098992, patch 14.2):

- `EXCEPTION_ACCESS_VIOLATION` at `Retail.exe+0x2158fee`, on the thread named `resFILE_LOADER`;
- `rcx = 3` at the fault: the object the code dereferences has been overwritten, and the value it finds where a
  pointer should be is a small integer, so the read faults at address `0xb` or `0x3b`;
- nothing in the kernel log, no GPU reset: a plain user-space fault.

Other people's minidumps in the issue show the same registers. It happens on vanilla Fedora, openSUSE, Arch and
NixOS kernels as well as CachyOS and Bazzite ones, with AMD and NVIDIA GPUs, with fsync as well as ntsync, and
with every Proton version tried. Reports cluster on Linux 7.2 kernels and several people found that 7.1 kernels
crash rarely or not at all; the Wine race described below does not depend on the kernel, so whatever 7.2 changed
only alters how often the race is lost. Game patch 15 (build 25648340, 2026-10-01) still crashes on stock Proton;
its offsets differ from the ones above.

## The game side

From a static analysis of `Retail.exe` build 25098992 (image base `0x140000000`):

- The function at `0x205ac90` builds a vector of 24-byte records on its own stack, one per file read, each with
  status 3 (pending), and registers a completion callback per record: a lambda capturing `[&vector, index]`
  whose invoke function is `0x2158fd0`, the crash site. On completion the callback sets `vector[index].status`
  to 4 or 5.
- It submits the batch (`0x216da90` returns a future: state byte at `+4`, a `CRITICAL_SECTION` at `+0x20`, a
  `CONDITION_VARIABLE` at `+0x48`, callbacks at `+0x50`) and waits with a single
  `if (state not in {2, 3}) SleepConditionVariableCS(INFINITE)` at `0x205af82`. There is no loop around the
  wait; 36 inlined future-wait sites in the binary share this pattern.
- The completer (`0x2175e80`, the file-loader path) holds the critical section while it runs the callbacks and
  then wakes the condition variable, so a genuine completion is safe. Only a spurious wakeup lets `0x205ac90`
  return while reads are still pending. Its frame is then reused, and when the read completes the callback
  writes the status into dead stack memory and follows the pointer it finds there: the crash, or silent
  corruption when the fault happens to be readable.
- Right before the wait, the function grows the record vector, which allocates through the game's
  `libtcmalloc_minimal.dll`. That is where the stale alert comes from, as the next section explains.

The missing loop makes the game fragile, and its developer has been told (Focus/Saber ticket
[12458](https://community.focus-entmt.com/focus-entertainment/space-marine-2/bugs/12458)). But the game has run
on Windows for a year without this crash, because Windows never delivers the spurious wakeup. Wine does, and that
divergence is what the patches fix.

## The Wine side

Wine implements `RtlWaitOnAddress()` with a per-thread alert (`NtAlertThreadByThreadId()` sets it,
`NtWaitForAlertByThreadId()` waits for it and clears it) and 256 hashed wait queues protected by spinlocks
(`dlls/ntdll/sync.c`). A waiter puts an entry holding the address and its thread id, a local variable on its own
stack, on the queue, compares the value, and calls `NtWaitForAlertByThreadId(address, timeout)`. A waker takes the
lock, finds the entries for the address, clears each entry's address and unlinks it, releases the lock, and then
alerts each thread.

Three things go wrong when a wait ends in the window between the dequeue and the alert:

1. **Use after return.** Since commit e00cbef06d ("ntdll: Pre-check entry->addr before taking a spin lock in
   RtlWaitOnAddress()", 2024-04-24) a waiter whose wait has ended checks the entry's address without the lock,
   skips the locked section when it is already cleared, and returns. The waker clears the address first and then
   calls `list_remove()` on the entry (and, in `RtlWakeAddressAll()`, reads the thread id after that). If the
   waker is preempted between the store and those accesses, the waiter is gone and the waker reads a `next`/`prev`
   pair (and a thread id) from whatever now occupies that stack, and writes through the two pointers. The window
   is a few instructions, so this is rare, but it is memory corruption on the path every SRW lock, critical
   section and condition variable wake takes. In the compiled Proton build the compiler had also moved
   `RtlWakeAddressSingle()`'s thread-id load after the store, so source order alone did not protect it.
2. **Lost wake.** The waiter reports `STATUS_TIMEOUT` although a wake was aimed at it. For
   `RtlWakeAddressSingle()` no other waiter receives that wake.
3. **Stale alert.** The alert arrives with nobody waiting for it and stays pending. The thread's next
   `NtWaitForAlertByThreadId()` returns at once, whatever it waits for: typically a
   `SleepConditionVariableCS(INFINITE)` on an unrelated condition variable, which wakes up spuriously.

Zero-timeout waits make the race common. They are queued like any other wait and time out immediately, so a
concurrent wake easily picks one. gperftools' `SpinLockDelay()`, used by the tcmalloc in the game, picks a random
back-off of up to about 16 ms and truncates it to whole milliseconds, so most of its delays become
`WaitOnAddress(..., 0)`. Every `WakeByAddressSingle()` of the releasing thread can then hit a waiter whose 0 ms wait
is timing out, and an instrumented `ntdll` counted thousands of such events in a single play session, all on
allocating threads. The thread that grows the record vector right before its infinite wait is one of them.

## What Windows does

Windows 8 and later use a wait block with a flag that the two sides exchange. A waiter whose kernel wait timed out
exchanges the flag; if a waker had already claimed the block, the waiter does a second, untimed
`NtWaitForAlertByThreadId()` to take the wake that is on its way. A waker that finds the waiter already timed out
does not alert it. (The YY-Thunks `WaitOnAddress` thunk, written after the Windows 8 `ntdll` code and using its
private names `RtlpWaitOnAddressWithTimeout` and `RtlpWaitOnAddressRemoveWaitBlock`, shows the protocol.) So on
Windows a raced single wake is still used up by the timed-out waiter, as in Wine, but the alert never stays
pending. The measurements below confirm that.

The documentation allows condition-variable waits to wake spuriously, lists "a previous wake on the same address
was abandoned" among the reasons `WaitOnAddress()` may return early, and says `WakeByAddressSingle()` wakes "the
first thread to wait". None of that describes a wake that ends the thread's next wait on something else.

## The fix

- **Patch 1**, `ntdll: Don't touch a RtlWaitOnAddress() waiter's entry after clearing its address.` The wakers
  read the thread id and unlink the entry first, and clear the address last, with release semantics
  (`WritePointerRelease`); the waiter reads it with acquire semantics. The unlocked fast path stays.
- **Patch 2**, `ntdll: Take the in-flight alert when a RtlWaitOnAddress() wait times out after being dequeued.`
  A waiter that finds its entry dequeued after a timeout has been woken: it takes the alert with an untimed
  `NtWaitForAlertByThreadId(NULL, NULL)` and returns `STATUS_SUCCESS`. This is the Windows behaviour and has no
  timing constants; the waker that dequeued the entry is committed to alerting the thread, so the wait is bounded
  by the waker's next few instructions.
- **Patch 3**, `ntdll/tests: Test that a raced RtlWaitOnAddress() wake leaves no alert pending.` In `ntdll:sync`:
  a thread loops `RtlWakeAddressSingle()` or `RtlWakeAddressAll()` on one address while the test does 20,000
  `RtlWaitOnAddress()` calls on it with a timeout of 0 or 100 ns; after each one `NtWaitForAlertByThreadId(NULL, 0)`
  must not report an alert pending. Without that export the fallback is a 100 ns wait on another address that must
  time out.

Earlier versions of the series, and why they were dropped: v2 consumed the alert but kept the unlocked check; v3
ignored alerts that arrived while the waiter was still queued, instead of consuming them; v4 bounded the consume
with a 1 ms grace period, ignored alerts while queued and special-cased zero timeouts. v4 deviates from Windows,
where `NtAlertThreadByThreadId()` does end a `RtlWaitOnAddress()` wait and zero-timeout waits are queued, so v5
went back to what Windows does and v6 (this series) added the mutex patches. Alerting under the queue lock would
also close the race, but puts a futex wake inside a spinlock on the path of every lock and condition variable
wake, and was rejected.

## Testing

### Test programs

[`repro/raced_wake_probe.c`](repro/raced_wake_probe.c) is the standalone form of the conformance test: a waker
thread hammers one address while the main thread does 0 ns and 100 ns waits on it; after each wait it either
checks for a pending alert (`probe` passes) or does a 100 ns wait on an unrelated address that must time out
(`early` passes). [`repro/stale_wake.c`](repro/stale_wake.c) is a stress program with the game's pattern: 8
workers each take a tcmalloc-style spinlock polled with `WaitOnAddress(..., 0)` and released with
`WakeByAddressSingle()` (4 more threads hammer the same lock), then do one unlooped
`SleepConditionVariableCS(INFINITE)` on a future that a dedicated thread completes, and count returns while the
future is still pending. Build and usage: [`repro/README.md`](repro/README.md).

The Windows machine is a Windows 11 Enterprise Evaluation 25H2 VM (build 26200.9457, QEMU/KVM, 8 vCPUs configured
of which Windows uses 4) on the same Ryzen 7 9800X3D host as the Wine runs, with the same binaries.

### Stock Wine against Windows

| Same binaries, same host | Windows 11 | Proton Experimental 11.0-20260924, stock |
|---|---:|---:|
| `raced_wake_probe 20000 3`: 0 ns waits that timed out while a waker was dequeuing them, of 20,000 (wake single / wake all) | 39 / 47 | 147 / 126 |
| ... that left an alert pending | 0 / 0 | 18 / 20 |
| 100 ns wait on another address returning early afterwards | 0 / 0 (189 samples: a 100 ns wait costs a timer tick on Windows) | 22 / 27 of 20,000 |
| `stale_wake 15 8 poll`: unlooped condition-variable waits returning while still pending | 0 of 100,097 | 69,195 of 529,155 (13%) |
| `stale_wake 15 8 both` / `timed` | 0 of 26,073 / 0 of 85,325 | 24,088 of 137,275 / 45 of 139,069 |

Stock Proton Experimental 11.0-20261001 (the base of this branch; the same `ntdll` code) gives the same picture:
107 pending alerts and 42 early returns over the probe's eight passes of 20,000 waits. Raw Windows output:
[`results/windows-2026-09-29.txt`](results/windows-2026-09-29.txt).

### Patched Wine

| Build | `stale_wake 15 8 poll` (waits / spurious) | `raced_wake_probe` (pending alerts / early returns) |
|---|---:|---:|
| Stock Experimental 20260924 | 529,155 / 69,195 | 41 / 59 |
| Stock Experimental 20261001 | | 107 / 42 |
| Patches 1–3 (v5) on 20260924 | 527,738 / 0 | 0 / 0 |
| Patches 1–5 (v6) on 20260924, 64-bit / 32-bit programs | 524,186 / 0 and 525,018 / 0 | 0 / 0 (32-bit probe) |
| Patches 1–5 on 20261001, 64-bit / 32-bit programs | 527,627 / 0 and 525,442 / 0 | 0 / 0 and 0 / 0 |

With the patches the race still occurs at the same rate (an instrumented build logged the "timed out after
dequeue, alert consumed" event up to 5,000 times in one game session); it just no longer leaves anything
behind.

### Wine's conformance tests

`ntdll:sync` (441 tests with the new one) and `kernel32:sync`, built from the patched tree and run as the same
binaries on Wine and on Windows:

| | `ntdll:sync` | `kernel32:sync` |
|---|---|---|
| Stock Proton Experimental 20260924 / 20261001 | 2 to 3 of the new test's 4 cases fail per run (23 to 245 of 20,000 raced waits leave an alert pending) | 2 failures in `test_mutex` (the [ntsync mutex issue](ntsync-mutex-abandonment.md), not this one) |
| Patches 1–3 | 0 failures, 3 runs | unchanged |
| Patches 1–5 | 0 failures | 0 failures |
| Windows 11 | 0 failures | 0 failures |

Raw output: [`results/windows-wine-tests-2026-09-29.txt`](results/windows-wine-tests-2026-09-29.txt).

### In the game

Before the patches, Space Marine 2 crashed regularly on my machine (Bazzite 44, Linux 7.2, Ryzen 7 9800X3D, ntsync),
with the signature above, as it does for the people in #8072. Since 2026-09-26, with the patched `ntdll` and
`wineserver` (first as replaced files in Steam's Experimental 20260924 and 20261001 builds, through every version of
the series), it has not crashed once: the game's crash bundle is unchanged since the last stock crash, on game
patch 14.2 and on patch 15. Several people on #8072 report the same with Jpokul's prototype build, which carries
the same patches on Experimental 20260924. One report of the prototype "not helping" turned out to be a freeze
without any access violation, a different problem.

## Upstream status

- 2026-09-26: analysis, crash data and the first prototype posted on Proton
  [#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5852529635); revised fix and in-game
  results on [09-27](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5858321198).
- 2026-09-27: Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397) filed, with the analysis and the
  reproducers described in prose.
- 2026-09-29: a Wine maintainer replied that the pending alert "is fundamentally part of the design of win32
  futexes and cannot be avoided". The Windows measurements above were made the same day and posted on the bug on
  09-30 and on [#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5903294489) with the
  probe's source; no further reply as of 2026-10-07. The bug is UNCONFIRMED.
- 2026-10-03: Valve ([kisak-valve](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5963964047))
  said that changes that can go to upstream Wine should go there first, with Wine's LLM policy in mind.
- Precedent: Wine merge request [3929](https://gitlab.winehq.org/wine/wine/-/merge_requests/3929) (Paul Gofman,
  merged 2024-04-24) fixed the other way a thread could be left alerted after its wait, a waiter alerted twice by
  `RtlWakeAddressAll()`; its description says it "fixes Resident Evil games randomly crashing due to unhandled
  spurious SleepConditionVariableCS wakeups". The same alert and the same symptom, with only the race that
  produces the stray alert differing. Its second commit is e00cbef06d, the pre-check behind point 1.
- 2026-10-07: [wine-cachyos PR #34](https://github.com/CachyOS/wine-cachyos/pull/34) (Erhan Bilgili) makes the
  same change as patch 1, in the same form, without citing the bug; open as of 2026-10-09.
- Wine's Developer FAQ says LLM-generated code is not accepted. The patches here were written with an LLM, so
  upstream has the bug reports, the measurements and the reproducers, and the patches are a reference for a fix
  that a Wine developer would write themselves.

## References

- Wine bug 60397: https://bugs.winehq.org/show_bug.cgi?id=60397
- Proton issue #8072: https://github.com/ValveSoftware/Proton/issues/8072
- Wine commit e00cbef06d (the unlocked pre-check): https://gitlab.winehq.org/wine/wine/-/commit/e00cbef06d
- `WaitOnAddress`: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress
- `WakeByAddressSingle`: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-wakebyaddresssingle
- `SleepConditionVariableCS`: https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablecs
- YY-Thunks `WaitOnAddress` (Windows 8 algorithm): https://github.com/Chuyu-Team/YY-Thunks/blob/master/src/Thunks/api-ms-win-core-synch.hpp
