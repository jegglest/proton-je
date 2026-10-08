# Issues this branch addresses

One file per issue, each with the symptom, the cause in Wine, what Windows does, the fix, the test results on
Linux and on Windows, and where things stand upstream. The test programs the write-ups quote are in
[`repro/`](repro), with build and usage notes; the raw output of the Windows runs is in [`results/`](results).

| Issue | Patches | Upstream | State |
|---|---|---|---|
| [Stale thread alerts from `RtlWaitOnAddress()`](stale-thread-alert.md): a wake racing a timed-out wait leaves an alert that ends the thread's next wait early; Space Marine 2 crashes mid-mission because of it | 1–3 (`ntdll`) | Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397), Proton issue [#8072](https://github.com/ValveSoftware/Proton/issues/8072) | UNCONFIRMED; a maintainer called the alert unavoidable, the Windows measurements say otherwise; no reply since |
| [ntsync mutexes are never abandoned once their handles are closed](ntsync-mutex-abandonment.md): a waiter blocks forever instead of getting `WAIT_ABANDONED`; the two `kernel32:sync` `test_mutex` failures | 4–5 (`wineserver`, `ntdll` unix side) | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) | UNCONFIRMED, no reply yet |
| [FSR 4 renders the FSR 3 model on RDNA4](fsr4-rdna4.md): Proton Experimental's `amdxc64` stub cannot tell AMD's FSR 4 DLL what the GPU supports, so Valve enables the upgrade on RDNA3 only | 6–31 (`amdxc64`, `win32u`), Etaash Mathamsetty's, from Proton-EM via Proton-GE | Proton issue [#9908](https://github.com/ValveSoftware/Proton/issues/9908) | open at Valve; fixed in Proton-EM, Proton-GE and here |

## How the measurements were made

Linux: Bazzite 44 (Fedora-based) with Linux 7.2.4 to 7.2.8, Ryzen 7 9800X3D, RX 9070 XT, `/dev/ntsync` in use.
Wine: Proton Experimental 11.0-20260924 and 11.0-20261001, stock and with the patches, the patched builds first
as replaced `ntdll`/`wineserver` files in Steam's own build and now as the full builds in this repository's
releases. Windows: Windows 11 Enterprise Evaluation 25H2, build 26200.9457, in a QEMU/KVM virtual machine on the
same host, running the same executables. Wine's conformance tests (`ntdll:sync`, `kernel32:sync`) were built from
the patched tree and run as the same binaries on both systems.

## Provenance

The investigation, the patches and the test programs were done with the help of an LLM (Claude); the
measurements, the in-game testing and the upstream reports are mine. Wine's contribution policy excludes
LLM-generated code from merge requests, so upstream has the bug reports with the analysis and reproducers, and
this repository carries the patches as a reference and so that people can test the fixes.
The FSR 4 commits are Etaash Mathamsetty's work from Proton-EM, taken from Proton-GE's patch set as they are;
only the `proton` script change and the write-up are mine, again with the LLM's help.

## Adding an issue

Add the commits to the `je-11.0` branch of [`jegglest/wine`](https://github.com/jegglest/wine) (or to this
repository for a change outside Wine) and point the `wine/` submodule at the new head, then a write-up here with
the same sections as the existing ones, its reproducer under [`repro/`](repro) if it has one, and a row in the
table above and in the top-level [README](../README.md).
