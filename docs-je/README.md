# What this branch carries

Three groups of changes, kept apart on purpose: patches meant for upstream Wine, which are mine, changes carried
from elsewhere that this repository does not try to upstream, and Valve's own fixes from its newer Wine, taken
before they reach Proton Experimental. Plus one thing that needed no patch at all. The test programs the write-ups
quote are in [`repro/`](repro), with build and usage notes; the raw output of the Windows runs is in
[`results/`](results).

## Patches for upstream Wine

Five commits in four self-contained pieces, found while tracking down the mid-mission crash of Warhammer
40,000: Space Marine 2 under Proton. Each piece has a page under [`patches/`](patches) with the commit and its
patch file, what it fixes, what it needs, where it applies, how to check it and where it stands upstream:

| Patch | Page | Commit | Touches | Upstream |
|---|---|---|---|---|
| 1 | [The wakers must not touch a waiter's entry after clearing its address](patches/waitonaddress-use-after-return.md) | [`waitonaddress-use-after-return`](https://github.com/jegglest/wine/commit/waitonaddress-use-after-return) | `dlls/ntdll/sync.c` | Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397), point 1. Fixed in Wine on 2026-10-14 by [`81e52628`](https://gitlab.winehq.org/wine/wine/-/commit/81e52628b25ac173d04796e8a9b9cda637d3e233) ([MR 12315](https://gitlab.winehq.org/wine/wine/-/merge_requests/12315), Wine 11.20), the same change; kept here until Valve's Wine has it. Also pending as [wine-cachyos PR #34](https://github.com/CachyOS/wine-cachyos/pull/34) |
| 2 and 3 | [Take the in-flight alert when a wait times out after being dequeued](patches/waitonaddress-stale-alert.md), with its conformance test | [`waitonaddress-stale-alert`](https://github.com/jegglest/wine/commit/waitonaddress-stale-alert), [`waitonaddress-stale-alert-test`](https://github.com/jegglest/wine/commit/waitonaddress-stale-alert-test) | `dlls/ntdll/sync.c`, `dlls/ntdll/tests/sync.c` | Wine bug 60397, points 2 and 3 |
| 4 | [Keep an owned ntsync mutex reachable after its server object is destroyed](patches/ntsync-mutex-owned-after-close.md) | [`ntsync-mutex-owned-after-close`](https://github.com/jegglest/wine/commit/ntsync-mutex-owned-after-close) | `server/inproc_sync.c` | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) |
| 5 | [Keep an in-process mutex that a process closed while still using it](patches/ntsync-mutex-in-use-after-close.md) | [`ntsync-mutex-in-use-after-close`](https://github.com/jegglest/wine/commit/ntsync-mutex-in-use-after-close) | `server/`, `dlls/ntdll/unix/` (server protocol version bump) | Wine bug 60417, second part |

Getting them as patches: the [compare view](https://github.com/jegglest/wine/compare/experimental_11.0...ntsync-mutex-in-use-after-close)
shows exactly these five commits, and GitHub generates the patch files from the commits on request: `.patch`
appended to any commit link above gives that commit as a `git am` file, and
[the compare link with `.patch` appended](https://github.com/jegglest/wine/compare/experimental_11.0...ntsync-mutex-in-use-after-close.patch)
gives the whole series in one file (`git format-patch experimental_11.0..ntsync-mutex-in-use-after-close` in a
clone does the same). No patch files are kept in this repository: the commits are the patches. The commit links
are tags on the fork, named after the pages, that move to the rebased commits with every new Proton
Experimental; the compare view's base is the fork's mirror of Valve's `experimental_11.0`. Patches 2 and 5
depend on 1 and 4 respectively; 1, 3 and 4 stand alone. Checked on 2026-10-09: all five apply with `git am` to
Valve's Wine `6d211aab` (and, rebased on 2026-10-14 and 2026-10-16, to `bc8d17c6` of Experimental 20261008 and
`7fc704ef` of Experimental 20261014) and to wine-cachyos (`cachyos_11.0_20261001/main` of 2026-10-04). On Wine
master, checked 2026-10-16 on `7298a818` (Release 11.20): 1 is there since `81e52628` of 2026-10-14, 2 applies on
top of it with `git am --3way` and 3 with `git am`, while 4 and 5 need a hand port because Proton's fsync fallback
sits on the lines they change.

The investigation behind them, one page per bug, with the symptom, the cause, what Windows does, the full test
results on Linux and on Windows, and the upstream history:

| Issue | Patches | Upstream | State |
|---|---|---|---|
| [Stale thread alerts from `RtlWaitOnAddress()`](stale-thread-alert.md): a wake racing a timed-out wait leaves an alert that ends the thread's next wait early; Space Marine 2 crashes mid-mission because of it | 1 to 3 (`ntdll`) | Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397), Proton issue [#8072](https://github.com/ValveSoftware/Proton/issues/8072) | UNCONFIRMED; a maintainer called the alert unavoidable, the Windows measurements say otherwise, and Wine fixed a sibling case in 2024 (merge request 3929, for Resident Evil); no reply since 2026-09-30 |
| [ntsync mutexes are never abandoned once their handles are closed](ntsync-mutex-abandonment.md): a waiter blocks forever instead of getting `WAIT_ABANDONED`; the two `kernel32:sync` `test_mutex` failures | 4 and 5 (`wineserver`, `ntdll` unix side) | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) | UNCONFIRMED, no reply yet |

## Carried, not for upstream

| What | Commits | Where from | Why it is here |
|---|---|---|---|
| [FSR 4 renders the FSR 3 model on RDNA4](fsr4-rdna4.md): Proton Experimental's `amdxc64` stub cannot tell AMD's FSR 4 DLL what the GPU supports, so Valve enables the upgrade on RDNA3 only | 6 to 31 on the Wine branch (`amdxc64`, `win32u`), plus the `proton` script's handling of the FSR 4 DLL in this repository | Etaash Mathamsetty's work from [Proton-EM](https://github.com/Etaash-mathamsetty/Proton), as [Proton-GE](https://github.com/GloriousEggroll/proton-ge-custom) ships it, applied unchanged with its authorship | so that the builds are nicer to test with on RDNA4 cards. These commits are not mine, nothing here tries to upstream them, and `amdxc64` is a Proton-only module in any case; Valve's position is in Proton issue [#9908](https://github.com/ValveSoftware/Proton/issues/9908) |

## Taken early from Valve's Wine

Valve's own fixes from its newer Proton branches, carried as they are, with their authorship and a line naming
where they come from, until they reach Proton Experimental; the rebase onto that Experimental then drops them.
They go last on the branch.

| What | Commit | What it changes | Upstream |
|---|---|---|---|
| [Helldivers 2 closes with `STATUS_INVALID_HANDLE`](helldivers2-renderer-race.md): the game's D3D12 task dispatcher leaves a critical section in a stack frame its renderer thread has already left | commit 32, [`cs-unwait-ignore-errors`](https://github.com/jegglest/wine/commit/cs-unwait-ignore-errors) (`dlls/ntdll/sync.c`), Paul Gofman's "ntdll: Ignore semaphore errors in RtlpUnWaitCriticalSection()" from Valve's bleeding-edge branch | `LeaveCriticalSection()` no longer raises when releasing the semaphore of a critical section fails. Release `-2` carried a workaround of its own instead, which kept the woken thread waiting until the dispatcher was out of `LeaveCriticalSection()` | in Valve's bleeding-edge branch since 2026-10-20, not yet in Proton Experimental; upstream Wine still raises there, as Windows does with a real waiter. What makes the game crash under Wine is that Wine encodes a critical section's state differently from Windows |

## Documented, nothing carried

| What | What to do |
|---|---|
| [AMD Anti-Lag 2 is refused under Proton](amd-anti-lag-2.md): Mesa's `VK_AMD_anti_lag` layer is off unless asked for, so vkd3d-proton tells games there is no Anti-Lag 2 | put `ENABLE_LAYER_MESA_ANTI_LAG=1` in the game's launch options; the same for every Proton, nothing to report upstream |

## How the measurements were made

Linux: Bazzite 44 (Fedora-based) with Linux 7.2.4 to 7.2.9, Ryzen 7 9800X3D, RX 9070 XT, `/dev/ntsync` in use.
Wine: Proton Experimental 11.0-20260924 and 11.0-20261001, stock and with the patches, the patched builds first
as replaced `ntdll`/`wineserver` files in Steam's own build and now as the full builds in this repository's
releases. Windows: Windows 11 Enterprise Evaluation 25H2, build 26200.9457, in a QEMU/KVM virtual machine on the
same host, running the same executables. Wine's conformance tests (`ntdll:sync`, `kernel32:sync`) were built from
the patched tree and run as the same binaries on both systems. The Helldivers 2 write-up reads the game's own
crash dumps, from Proton Experimental 11.0-20261014 and `proton-je-11.0-20261014-1`; its probe of
`LeaveCriticalSection()` ran on Linux 7.2.9 and on the Windows virtual machine above.

## Provenance

The investigation, the patches and the test programs were done with the help of an LLM (Claude); the
measurements, the in-game testing and the upstream reports are mine. Wine's contribution policy excludes
LLM-generated code from merge requests, so upstream has the bug reports with the analysis and reproducers, and
this repository carries the patches as a reference and so that people can test the fixes.
The FSR 4 commits are Etaash Mathamsetty's work from Proton-EM, taken from Proton-GE's patch set as they are;
only the `proton` script change and the write-up are mine, again with the LLM's help. Commit 32 is Paul Gofman's,
from Valve's Wine.

## Adding a patch or an issue

Add the commits to the `je-11.0` branch of [`jegglest/wine`](https://github.com/jegglest/wine) (or to this
repository for a change outside Wine), keeping the original author and, for someone else's commits, ending each
message with a line that names them and where the commits come from, as the FSR 4 commits do. Point the `wine/`
submodule at the new head. For a patch meant for upstream, add a page under [`patches/`](patches) with the same rows
and sections as the existing ones, one page per self-contained piece (a piece being what could be sent upstream on
its own), and a row in the upstream table above; for a change carried from elsewhere, keep it out of that table and
say where it comes from and why it is here; for a fix taken early from Valve's Wine, put it last on the branch, keep
its author and end its message with a line saying it is their commit from Valve's Wine and which branch it comes
from, which is how `.github/release-notes.sh` lists it apart, and add a row to that table above; for a game
workaround of this repository's own, put the commit last with `HACK:` in its subject, as Valve does, which
`.github/release-notes.sh` lists apart too, and give it a table here. Then add the issue write-up with the same
sections as the existing ones, its reproducer under [`repro/`](repro) if it has one, and a row in the top-level
[README](../README.md).