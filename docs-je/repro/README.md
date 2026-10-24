# Test programs

Small Windows programs that show the issues in [`docs-je/`](..) without running a game, and that behave the same way
on Windows, so that Wine can be compared with it. The write-ups quote their results.

| Program | Issue | What it does |
|---|---|---|
| [`raced_wake_probe.c`](raced_wake_probe.c) | [Stale thread alerts](../stale-thread-alert.md) | A waker thread hammers `RtlWakeAddressSingle()` / `RtlWakeAddressAll()` on one address while the main thread does 0 ns and 100 ns `RtlWaitOnAddress()` waits on it, so that wakes race the waits timing out. After each wait it checks for a pending alert (`NtWaitForAlertByThreadId(NULL, 0)`), or does a 100 ns wait on an unrelated address that must time out. The same measurement as the conformance test in patch 3. |
| [`stale_wake.c`](stale_wake.c) | [Stale thread alerts](../stale-thread-alert.md) | Stress program with the game's pattern: workers take a tcmalloc-style spinlock polled with `WaitOnAddress(..., 0)` and then do one unlooped `SleepConditionVariableCS(INFINITE)` on a future that another thread completes. Counts waits that return while the future is still pending. |
| [`mutex_abandon.c`](mutex_abandon.c) | [ntsync mutex abandonment](../ntsync-mutex-abandonment.md) | Timed version of the end of Wine's `kernel32:sync` `test_mutex()`, plus the wait-all corner case: a thread owning a mutex is killed with `TerminateThread()` after the mutex handle was closed, and a waiter must get `WAIT_ABANDONED`. |
| [`amd_probe.c`](amd_probe.c) | [AMD Anti-Lag 2](../amd-anti-lag-2.md), [FSR 4 on RDNA4](../fsr4-rdna4.md) | Creates a D3D12 device and asks the `amdxc64` driver shim for what AMD's SDKs use: the Anti-Lag 2 interface (then the SDK's off, on and frame-marker calls), the `IAmdExtD3DFactory` interfaces FSR 4 queries for wave-matrix and FP8 support, and with `--ffx` the FSR 3.1 to FSR 4 provider upgrade itself. Prints one line per step and writes the same to `amd_probe_out.txt`. |
| [`cs_leave_bad_semaphore.c`](cs_leave_bad_semaphore.c) | [Helldivers 2 renderer race](../helldivers2-renderer-race.md) | What `LeaveCriticalSection()` does when it has a waiter to wake and `LockSemaphore` is not a valid handle, three ways, each in a process of its own: `waiter` (a second thread blocked in `EnterCriticalSection()`, then `LockSemaphore` set to `0xdeadbeef`), `stale` (the fields hold pointers between 4 and 8 GB, as a reused stack frame does under Wine) and `stale-high` (pointers where Windows puts them). Prints the fields before and after and whether the leave returned or raised. |
| [`inproc_cache_probe.c`](inproc_cache_probe.c) | [ntsync mutex abandonment](../ntsync-mutex-abandonment.md), patch 5 | Creates 5000 events (or the count given) and waits on each one once, so that `ntdll`'s in-process sync cache has to map a second block of entries. Checks the page-size slip of the first version of patch 5, which aborted releases `-1` and `-2` of this repository once a game had about 3300 handles open. Prints the highest handle value and one line per 1000 waits. |

## Building

With MinGW-w64 (Fedora: `mingw64-gcc` and `mingw32-gcc`):

```bash
x86_64-w64-mingw32-gcc -O2 -D__USE_MINGW_ANSI_STDIO=1 -o stale_wake.exe stale_wake.c -lsynchronization
x86_64-w64-mingw32-gcc -O2 -D__USE_MINGW_ANSI_STDIO=1 -o raced_wake_probe.exe raced_wake_probe.c
x86_64-w64-mingw32-gcc -O2 -D__USE_MINGW_ANSI_STDIO=1 -o mutex_abandon.exe mutex_abandon.c
x86_64-w64-mingw32-gcc -O1 -Wno-format -o amd_probe.exe amd_probe.c
x86_64-w64-mingw32-gcc -O2 -o inproc_cache_probe.exe inproc_cache_probe.c
x86_64-w64-mingw32-gcc -O2 -o cs_leave_bad_semaphore.exe cs_leave_bad_semaphore.c
```

The same commands with `i686-w64-mingw32-gcc` give 32-bit builds, which matter for the mutex issue: under Proton a
32-bit process uses the 32-bit unix `ntdll.so`, and the server protocol change in patch 5 has to be present there too.

## Running

```
raced_wake_probe.exe [iterations per pass, default 20000] [seconds cap per pass, default 3]
stale_wake.exe [seconds, default 30] [workers, default 8] [timed|poll|both, default both]
mutex_abandon.exe [close|keep|waitall, default close] [iterations, default 1]
inproc_cache_probe.exe [events, default 5000]
cs_leave_bad_semaphore.exe [waiter|stale|stale-high, default all three]
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
`mutex_abandon` needs ntsync to show the Wine bug (`/dev/ntsync`, Linux 6.14 or later). Proton's fsync fallback has the
same gap, so under the `proton` script, which sets `WINEFSYNC=1`, `PROTON_NO_NTSYNC=1` alone only moves the hang to
fsync; with `PROTON_NO_NTSYNC=1 PROTON_NO_FSYNC=1` the server-side mutexes are used and the bug does not occur. The
standalone `files/bin/wine` runs above use fsync only if `WINEFSYNC=1` is set.

`inproc_cache_probe` must end with `waited on all 5000 events: OK`, on Windows and on Wine. Releases `-1` and
`-2` of this repository abort it at the wait on the handle with index 3276, with
``virtual.c:408: anon_mmap_alloc: Assertion `!(size & host_page_mask)' failed.`` on stderr, and the process may then
hang rather than exit; 3000 events pass there. It needs `/dev/ntsync` in use, like `mutex_abandon`.

`cs_leave_bad_semaphore` prints the Windows or Wine version and one line per mode. With Proton Experimental
11.0-20261014 and `proton-je-11.0-20261014-2`, `waiter` and `stale` print `LeaveCriticalSection() raised 0xc0000008,
flags 0x1` and `stale-high` prints `returned`; from `proton-je-11.0-20261014-3` on, which carries Valve's fix, all
three return.

`amd_probe.exe` needs vkd3d-proton's `d3d12.dll` and the FSR 4 DLL in the prefix, which only the tool's `proton`
script sets up, so run it the way Steam would, with a scratch compatdata directory:

```bash
ENABLE_LAYER_MESA_ANTI_LAG=1 STEAM_COMPAT_DATA_PATH=/tmp/je-probe STEAM_COMPAT_CLIENT_INSTALL_PATH=~/.local/share/Steam \
    STEAM_COMPAT_LIBRARY_PATHS=~/.local/share/Steam/steamapps SteamGameId=0 PROTON_LOG=+amdxc PROTON_LOG_DIR=$PWD \
    $T/proton run amd_probe.exe --ffx
```

Expected on an RDNA4 card with the Mesa anti-lag layer installed and enabled as above: `IAmdExtAntiLagApi: 0`, three `S_OK`
`UpdateAntiLagState()` lines, `IAmdExtD3DDevice8: 0` with one 16x16x16 FP8 (type 11) wave-matrix entry, and
`CheckSupport(WaveMatrix)` and `CheckSupport(Float8Conversion)` returning 0. Without the Proton-EM `amdxc64`
the factory interfaces are stubs and `IAmdExtD3DDevice8` is refused; without `ENABLE_LAYER_MESA_ANTI_LAG=1` the
Anti-Lag interface is refused.
