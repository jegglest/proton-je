# Test programs

Small Windows programs that show the issues in [`docs-je/`](..) without running a game, and that behave the same way
on Windows, so that Wine can be compared with it. The write-ups quote their results.

| Program | Issue | What it does |
|---|---|---|
| [`raced_wake_probe.c`](raced_wake_probe.c) | [Stale thread alerts](../stale-thread-alert.md) | A waker thread hammers `RtlWakeAddressSingle()` / `RtlWakeAddressAll()` on one address while the main thread does 0 ns and 100 ns `RtlWaitOnAddress()` waits on it, so that wakes race the waits timing out. After each wait it checks for a pending alert (`NtWaitForAlertByThreadId(NULL, 0)`), or does a 100 ns wait on an unrelated address that must time out. The same measurement as the conformance test in patch 3. |
| [`stale_wake.c`](stale_wake.c) | [Stale thread alerts](../stale-thread-alert.md) | Stress program with the game's pattern: workers take a tcmalloc-style spinlock polled with `WaitOnAddress(..., 0)` and then do one unlooped `SleepConditionVariableCS(INFINITE)` on a future that another thread completes. Counts waits that return while the future is still pending. |
| [`mutex_abandon.c`](mutex_abandon.c) | [ntsync mutex abandonment](../ntsync-mutex-abandonment.md) | Timed version of the end of Wine's `kernel32:sync` `test_mutex()`, plus the wait-all corner case: a thread owning a mutex is killed with `TerminateThread()` after the mutex handle was closed, and a waiter must get `WAIT_ABANDONED`. |
| [`amd_probe.c`](amd_probe.c) | [AMD Anti-Lag 2](../amd-anti-lag-2.md), [FSR 4 on RDNA4](../fsr4-rdna4.md) | Creates a D3D12 device and asks the `amdxc64` driver shim for what AMD's SDKs use: the Anti-Lag 2 interface (then the SDK's off, on and frame-marker calls), the `IAmdExtD3DFactory` interfaces FSR 4 queries for wave-matrix and FP8 support, and with `--ffx` the FSR 3.1 to FSR 4 provider upgrade itself. Prints one line per step and writes the same to `amd_probe_out.txt`. |

## Building

With MinGW-w64 (Fedora: `mingw64-gcc` and `mingw32-gcc`):

```bash
x86_64-w64-mingw32-gcc -O2 -D__USE_MINGW_ANSI_STDIO=1 -o stale_wake.exe stale_wake.c -lsynchronization
x86_64-w64-mingw32-gcc -O2 -D__USE_MINGW_ANSI_STDIO=1 -o raced_wake_probe.exe raced_wake_probe.c
x86_64-w64-mingw32-gcc -O2 -D__USE_MINGW_ANSI_STDIO=1 -o mutex_abandon.exe mutex_abandon.c
x86_64-w64-mingw32-gcc -O1 -Wno-format -o amd_probe.exe amd_probe.c
```

The same commands with `i686-w64-mingw32-gcc` give 32-bit builds, which matter for the mutex issue: under Proton a
32-bit process uses the 32-bit unix `ntdll.so`, and the server protocol change in patch 5 has to be present there too.

## Running

```
raced_wake_probe.exe [iterations per pass, default 20000] [seconds cap per pass, default 3]
stale_wake.exe [seconds, default 30] [workers, default 8] [timed|poll|both, default both]
mutex_abandon.exe [close|keep|waitall, default close] [iterations, default 1]
```

On Linux, run them with the Wine of the Proton build under test, for example from an extracted release:

```bash
export WINEPREFIX=/tmp/je-test LD_LIBRARY_PATH=$T/files/lib64:$T/files/lib WINEDLLOVERRIDES="mscoree,mshtml="
$T/files/bin/wine raced_wake_probe.exe 20000 3
$T/files/bin/wine stale_wake.exe 15 8 poll
$T/files/bin/wine mutex_abandon.exe close 5
```

where `T` is the tool's directory. On Windows, run the same executables from a command prompt. The expected results,
on Windows and on a patched Wine, are 0 stale alerts and 0 early returns from the probe, 0 spurious returns from
`stale_wake`, and `0 failed step(s)` with `returned 0x80 (abandoned)` lines from `mutex_abandon` in all three modes.
`mutex_abandon` needs ntsync to show the Wine bug (`/dev/ntsync`, Linux 6.14 or later); with `PROTON_NO_NTSYNC=1`
the server-side mutexes are used and the bug does not occur.

`amd_probe.exe` needs vkd3d-proton's `d3d12.dll` and the FSR 4 DLL in the prefix, which only the tool's `proton`
script sets up, so run it the way Steam would, with a scratch compatdata directory:

```bash
STEAM_COMPAT_DATA_PATH=/tmp/je-probe STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.local/share/Steam \
    STEAM_COMPAT_LIBRARY_PATHS=~/.local/share/Steam/steamapps SteamGameId=0 PROTON_LOG=+amdxc PROTON_LOG_DIR=$PWD \
    $T/proton run amd_probe.exe --ffx
```

Expected on an RDNA4 card with the Mesa anti-lag layer installed: `IAmdExtAntiLagApi: 0`, three `S_OK`
`UpdateAntiLagState()` lines, `IAmdExtD3DDevice8: 0` with one 16x16x16 FP8 (type 11) wave-matrix entry, and
`CheckSupport(WaveMatrix)` and `CheckSupport(Float8Conversion)` returning 0. Without the Proton-EM `amdxc64`
the factory interfaces are stubs and `IAmdExtD3DDevice8` is refused; without the anti-lag layer the Anti-Lag
interface is refused.
