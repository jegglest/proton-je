# Patch 1: the wakers must not touch a waiter's entry after clearing its address

| | |
|---|---|
| Commit | [`f81587bf7d`](https://github.com/jegglest/wine/commit/f81587bf7d9fba5a0271b73d883942a67d7d4f7b) `ntdll: Don't touch a RtlWaitOnAddress() waiter's entry after clearing its address.` ([patch file](https://github.com/jegglest/wine/commit/f81587bf7d9fba5a0271b73d883942a67d7d4f7b.patch)) |
| Changes | `dlls/ntdll/sync.c`, 12 lines added and 5 removed, in `RtlWaitOnAddress()`, `RtlWakeAddressSingle()` and `RtlWakeAddressAll()` |
| Fixes | memory corruption: a waker can still be reading and writing a waiter's stack entry after the waiter has returned and its caller has reused that stack |
| Needs | nothing; it stands on its own. [Patch 2](waitonaddress-stale-alert.md) is written on top of it |
| Applies to | Valve's Wine [`6d211aab`](https://github.com/ValveSoftware/wine/commit/6d211aabd1d325c9990a5fd7a81b9db37caae12d) (Proton Experimental 20261001), Wine master as of 2026-10-09 (`a15ee2ccf2`) and wine-cachyos `cachyos_11.0_20261001/main` as of 2026-10-04 (`2ce6b44caa`), with `git am`, no conflicts |
| Upstream | Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397), point 1 of the report; UNCONFIRMED. The same change is pending independently as [wine-cachyos PR #34](https://github.com/CachyOS/wine-cachyos/pull/34) (2026-10-07, open) |
| Windows | no equivalent code path: its wait block is handed from waiter to waker with a flag exchange, not through a list entry the waker keeps touching |
| Full story | [Stale thread alerts from `RtlWaitOnAddress()`](../stale-thread-alert.md), section "The Wine side" |

## The bug

`RtlWaitOnAddress()` puts an entry holding the address and its thread id on a hashed wait queue; the entry is a
local variable on the waiter's stack. `RtlWakeAddressSingle()` and `RtlWakeAddressAll()` find it under the
queue's spinlock, clear its address, then unlink it with `list_remove()`, which reads the entry's `next` and
`prev` and writes through them; `RtlWakeAddressAll()` reads the thread id after that too. Since Wine commit
[e00cbef06d](https://gitlab.winehq.org/wine/wine/-/commit/e00cbef06d) (2024, "ntdll: Pre-check entry->addr
before taking a spin lock in RtlWaitOnAddress()") a waiter whose wait has ended looks at the address without
the lock and returns at once when it is already cleared. If the waker is preempted between its store and the
accesses that follow it, the waiter is gone, its caller reuses the stack, and the waker reads a `next`/`prev`
pair (and a thread id) from whatever is there now and writes through the two pointers.

The window is a few instructions and needs the wait to end on its own inside it, on a timeout or on an alert
left pending by an earlier wait, so it is rare. But it is memory corruption on the path every critical section,
SRW lock and condition variable wake goes through. In the compiled Proton build the compiler had also moved
`RtlWakeAddressSingle()`'s thread-id load after the store, so source order alone did not protect it.

## The change

Both wakers read the thread id and unlink the entry first, then clear the address with `WritePointerRelease()`
as their last access to it; the waiter reads the address with `ReadPointerAcquire()`. Once a waiter sees the
address cleared, the waker is done with the entry. The unlocked fast path of e00cbef06d stays: a woken thread
still returns without touching the spinlock.

The other correct form is to take the lock unconditionally again, which is what the code did before
e00cbef06d: a waker holding the lock cannot race the waiter's return. It costs one spinlock round trip per wake,
and after a wake-all every woken thread takes the same lock at once. An earlier version of this series did it
that way, and the review of wine-cachyos PR #34 raised the same alternative. Either form fixes the bug; this one
keeps the optimisation that was added in the same merge request as the pre-check.

## How to check it

There is no deterministic reproducer: the window is a few instructions and needs a preemption inside it. What
can be checked:

- The diff: after the change no access to `entry` follows the release store in either waker, and the waiter's
  unlocked read is an acquire load.
- Nothing regresses: Wine's `ntdll:sync` (441 tests) and `kernel32:sync` pass on the patched build, on Wine and
  on Windows 11 with the same binaries.
- The path is exercised hard by the programs in [`repro/`](../repro): `raced_wake_probe.exe` races wakes against
  timed-out waits 160,000 times per run, and `stale_wake.exe 15 8 poll` makes about 525,000 condition-variable
  waits in 15 s on top of a polled spinlock, in 64- and 32-bit builds, without a crash on the patched build.

## Upstream

- Reported as point 1 of Wine bug 60397 on 2026-09-27 and restated with both possible fixes in
  [comment 2](https://bugs.winehq.org/show_bug.cgi?id=60397#c2) on 2026-09-30. The maintainer's reply (comment
  1) is about the alert of patch 2, not about this; no reply on this point.
- The unlocked pre-check that opened the window is the second commit of Wine merge request
  [3929](https://gitlab.winehq.org/wine/wine/-/merge_requests/3929) (Paul Gofman, merged 2024-04-24), added as
  an optimisation next to a fix for a spurious-wakeup crash in Resident Evil; its review did not discuss it.
- [wine-cachyos PR #34](https://github.com/CachyOS/wine-cachyos/pull/34) (Erhan Bilgili, 2026-10-07) makes the
  same change in the same form without citing the bug; as of 2026-10-09 it is open and its reviewers asked what
  it fixes.
- The patch was written with an LLM, which Wine's contribution policy excludes from merge requests, so it is a
  reference for a fix that a Wine developer would write themselves; the bug report has the analysis in prose.
