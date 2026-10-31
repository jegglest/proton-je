# Patches 2 and 3: take the alert of a wake that races a timeout

| | |
|---|---|
| Commits | [`waitonaddress-stale-alert`](https://github.com/jegglest/wine/commit/waitonaddress-stale-alert) `ntdll: Take the alert of a RtlWaitOnAddress() wake that races a timeout.` ([patch file](https://github.com/jegglest/wine/commit/waitonaddress-stale-alert.patch)); [`waitonaddress-stale-alert-test`](https://github.com/jegglest/wine/commit/waitonaddress-stale-alert-test) `ntdll/tests: Test a RtlWaitOnAddress() wake racing a timed-out wait.` ([patch file](https://github.com/jegglest/wine/commit/waitonaddress-stale-alert-test.patch)); the tags move to the rebased commits with every new Proton Experimental |
| Changes | `dlls/ntdll/sync.c`, 8 lines added and 3 removed: `RtlWaitOnAddress()`, and the comment in `RtlWakeAddressAll()` that the change makes obsolete; `dlls/ntdll/tests/sync.c`, 70 lines added: `test_wait_on_address_raced_wake()` in `ntdll:sync` |
| Fixes | a wake that races the end of a timed-out wait leaves an alert pending on the thread, and the thread's next wait on anything returns early: `SleepConditionVariableCS()`, `SleepConditionVariableSRW()`, SRW locks and critical sections all wait through the same alert. This is the mid-mission crash of Warhammer 40,000: Space Marine 2 under Proton |
| Needs | the change of [patch 1](waitonaddress-use-after-return.md), textually (the hunk sits inside it): in Wine's own form, `81e52628`, which Valve's Wine has since Experimental 20261027b and Wine master since 2026-10-14, and which the commit is now written on; the logic does not depend on it |
| Applies to | Valve's Wine [`7b3ceaf8`](https://github.com/ValveSoftware/wine/commit/7b3ceaf8644bb5d57140903e12897efac69be16a) (Proton Experimental 20261027b), with `git am`, no conflicts; Wine master too: rebased the same way onto Valve's bleeding-edge Wine, which had Wine's form of patch 1 first, both commits applied to master `66caeb46` (2026-10-28) with plain `git am`. Before, on top of patch 1: Valve's Wine [`6d211aab`](https://github.com/ValveSoftware/wine/commit/6d211aabd1d325c9990a5fd7a81b9db37caae12d), [`bc8d17c6`](https://github.com/ValveSoftware/wine/commit/bc8d17c6914a2285f397b71aa8bd73cd90377230) and [`7fc704ef`](https://github.com/ValveSoftware/wine/commit/7fc704ef49c50e9702c9983a5250d5738697bb04), and wine-cachyos of 2026-10-04 |
| Upstream | Wine bug [60523](https://bugs.winehq.org/show_bug.cgi?id=60523), filed 2026-10-17 from points 2 and 3 of bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397); both UNCONFIRMED. A maintainer replied on 60397 that the alert "is fundamentally part of the design of win32 futexes and cannot be avoided"; the Windows measurements below say otherwise, Wine's own comment in `RtlWakeAddressAll()` calls it the price of alerting outside the spinlock, and Wine fixed a sibling case of the same alert in 2024 (merge request 3929); see below |
| Windows | 0 pending alerts in 20,000 raced waits and 0 early returns in 100,000 unlooped condition-variable waits, same binaries, Windows 11 25H2; stock Wine: 18 to 20 pending alerts and 13% early returns. The current test passes there, 64- and 32-bit, 3 of 3 runs each (2026-10-28) |
| Full story | [Stale thread alerts from `RtlWaitOnAddress()`](../stale-thread-alert.md) |

## The bug

`RtlWakeAddressSingle()` and `RtlWakeAddressAll()` take a waiter's entry off the queue under the spinlock and
alert its thread (`NtAlertThreadByThreadId()`) only after releasing the lock. A waiter whose timeout expires in
between finds its entry dequeued but returns `STATUS_TIMEOUT`. The alert then arrives with nobody waiting for
it and stays pending on the thread, and the thread's next `NtWaitForAlertByThreadId()` returns at once,
whatever it is waiting for. Zero-timeout waits make this common: they are queued like any other wait and time
out immediately, and gperftools' spinlock back-off, used by the tcmalloc in Space Marine 2, polls with
`WaitOnAddress(..., 0)` most of the time. Such a thread collects a stale alert, and its next
`SleepConditionVariableCS(INFINITE)` on an unrelated condition variable wakes up spuriously.

In Space Marine 2 the function that waits for a batch of file reads does so with a single unlooped
`SleepConditionVariableCS(INFINITE)`. A spurious wakeup lets it return while reads are still pending, its stack
frame is reused, and the completion callback then writes into dead stack memory: the
`EXCEPTION_ACCESS_VIOLATION` on the `resFILE_LOADER` thread reported in Proton issue
[#8072](https://github.com/ValveSoftware/Proton/issues/8072). The game has run on Windows for a year without
that crash, because Windows never delivers the spurious wakeup.

## The change

Patch 2: a waiter that finds its entry already dequeued when its wait returns `STATUS_TIMEOUT` has been woken,
and the waker that dequeued it is about to alert it. The waiter takes that alert with an untimed
`NtWaitForAlertByThreadId(NULL, NULL)` and returns its status, `STATUS_ALERTED`, which the function already
reports as `STATUS_SUCCESS`: one line of code. The wakers do not change and there are no timing constants: the
waker alerts within its next few instructions after releasing the lock, which bounds the second wait. This is
what Windows does. Its wait block carries a flag the two sides exchange: a waiter that timed out and finds the
block already claimed by a waker does a second, untimed wait for the wake that is on its way, and a waker that
finds the waiter already timed out does not alert it (the YY-Thunks `WaitOnAddress`, written after the Windows 8
code, shows the protocol). The patch also replaces the end of the comment in `RtlWakeAddressAll()`, "(even if that
can be responsible for spurious wake up scenario)", which Paul Gofman wrote with `82275b13a24` (Wine 10.19) and
which describes this very wake: the wakers still alert outside the spinlock, and the waiter now takes the alert.

Patch 3: `test_wait_on_address_raced_wake()` in `ntdll:sync`. A thread loops `RtlWakeAddressSingle()` or
`RtlWakeAddressAll()` on one address while the test makes up to 20,000 `RtlWaitOnAddress()` calls on it with a
timeout of 0 or 100 ns; after each call `NtWaitForAlertByThreadId(NULL, 0)` must not report an alert pending. It
skips where either function is missing, i.e. before Windows 8, which introduced both. Stock Wine fails 2 or 3 of
the 4 cases per run, with 23 to 2,492 of 20,000 waits leaving an alert pending (Proton Experimental 20260924 to
20261014); the patched build and Windows 11 pass.

Earlier forms of the fix, dropped: ignoring alerts that arrive while still queued (Windows does return on
them), a 1 ms grace period for the consume (Windows uses an untimed wait), special-casing zero timeouts
(Windows queues them too), and alerting under the queue lock (a futex wake inside a spinlock on every lock and
condition-variable wake).

## How to check it

With the programs in [`repro/`](../repro) (build and run notes there), same binaries on Wine and on Windows:

| | Windows 11 | Stock Proton Experimental 20260924 | Patched (patches 1 to 3, or 1 to 6) |
|---|---:|---:|---:|
| `raced_wake_probe 20000 3`: raced 0 ns waits that left an alert pending (wake single / wake all) | 0 / 0 | 18 / 20 | 0 / 0 |
| same, 100 ns wait on another address returning early afterwards | 0 / 0 | 22 / 27 | 0 / 0 |
| `stale_wake 15 8 poll`: unlooped condition-variable waits that returned while still pending | 0 of 100,097 | 69,195 of 529,155 (13%) | 0 of 527,738 |
| `ntdll:sync` | 0 failures | 2 to 3 of the new test's 4 cases fail | 0 failures |

Raw Windows output: [`results/windows-2026-09-29.txt`](../results/windows-2026-09-29.txt) and
[`results/windows-wine-tests-2026-09-29.txt`](../results/windows-wine-tests-2026-09-29.txt). The current form of
the commits (release `-4`), measured on Proton Experimental 20261014 with ntsync: the new test fails 2 or 3 of its
4 cases in each of 3 runs on stock (41 to 2,492 of 20,000 waits leave an alert pending) and passes with the patches,
5 runs of the 64-bit test and 2 of the 32-bit one; `raced_wake_probe 20000 3` gives 65 stale alerts and 101 early
returns on stock, 0 and 0 with the patches, 64- and 32-bit. On Windows 11 the same test binaries pass, 64- and
32-bit, 3 of 3 runs each: [`results/windows-wine-tests-2026-10-28.txt`](../results/windows-wine-tests-2026-10-28.txt). With the patch the
race still happens at the same rate; it just no longer leaves anything behind. In the game: Space Marine 2 has
not crashed on the patched builds since 2026-09-26 on the machine the measurements were made on, after crashing
regularly on stock Proton, and several people on #8072 report the same with Jpokul's prototype build, which
carries the same patches.

## Upstream

- 2026-09-26: analysis and first prototype on Proton
  [#8072](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5852529635).
- 2026-09-27: Wine bug 60397 filed. 2026-09-29: a maintainer replied that the pending alert "is fundamentally
  part of the design of win32 futexes and cannot be avoided". 2026-09-30:
  [comment 2](https://bugs.winehq.org/show_bug.cgi?id=60397#c2) with the Windows measurements and the Windows
  protocol; no reply since.
- 2026-10-03: Valve ([kisak-valve](https://github.com/ValveSoftware/Proton/issues/8072#issuecomment-5963964047))
  asked that changes meant for upstream Wine go there first.
- 2026-10-17: bug 60397 split into narrower reports; points 2 and 3 are now Wine bug [60523](https://bugs.winehq.org/show_bug.cgi?id=60523), which
  links these commits. UNCONFIRMED, no reply as of 2026-10-28.
- Wine's own words: the comment Paul Gofman put above the alert in `RtlWakeAddressAll()` with `82275b13a24`
  (Wine 10.19), "Try not to make a system call while holding a spinlock (even if that can be responsible for
  spurious wake up scenario).", describes this wake as the known cost of alerting outside the lock, a trade-off
  rather than something the design makes unavoidable. Patch 2 keeps the alert outside the lock and removes the
  cost.
- Precedent: Wine merge request [3929](https://gitlab.winehq.org/wine/wine/-/merge_requests/3929) (Paul Gofman,
  merged 2024-04-24) fixed the other way a thread could be left alerted after its wait, a waiter alerted twice
  by `RtlWakeAddressAll()`. Its description says it "fixes Resident Evil games randomly crashing due to
  unhandled spurious SleepConditionVariableCS wakeups": the same alert, the same symptom and the same kind of
  crash, with only the race that produces the stray alert differing.
- The patches were written with an LLM, which Wine's contribution policy excludes from merge requests, so they
  are a reference for a fix that a Wine developer would write themselves; the analysis, the measurements and
  the test are described in the bug.
