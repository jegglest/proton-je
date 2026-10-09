Proton-JE
=========

Valve's [Proton Experimental](https://github.com/ValveSoftware/Proton/tree/experimental_11.0), built the way
Valve builds Proton, with two kinds of changes on top that are kept apart: the patches I am trying to get into
upstream Wine, five commits for two Wine bugs, so that other people can test the fixes before they land; and,
carried as they are from Proton-EM and not for upstreaming, the FSR 4 driver-side commits that make FSR 4 work
on RDNA4 cards. Nothing else is changed: no wine-staging, no other custom patches.

| | |
|---|---|
| Base | Proton `experimental_11.0` commit [`70b7e109`](https://github.com/ValveSoftware/Proton/commit/70b7e109e8fc0783a509805f3d1bc090b55dac3f) (Steam's `experimental-11.0-20261001`) with Wine [`6d211aab`](https://github.com/ValveSoftware/wine/commit/6d211aabd1d325c9990a5fd7a81b9db37caae12d) |
| Patches | the commits on branch [`je-11.0` of `jegglest/wine`](https://github.com/jegglest/wine/commits/je-11.0): Valve's Wine for this Experimental, then commits 1 to 5, my synchronization fixes meant for upstream Wine, then commits 6 to 31, Etaash Mathamsetty's FSR 4 work carried from Proton-EM; the `wine/` submodule points at it |
| Builds | [Releases](https://github.com/jegglest/proton-je/releases): `proton-je-<Proton version>-<n>.tar.xz`, built by [GitHub Actions](.github/workflows/build.yml) from the tagged commit |
| Write-ups | [`docs-je/`](docs-je): one page per patch for upstream ([`docs-je/patches/`](docs-je/patches)) with the commit, the patch file and how to check it; the full investigation of each issue, with the test results on Linux and on Windows |
| Discussion | Proton issue [#8072](https://github.com/ValveSoftware/Proton/issues/8072), Wine bugs [60397](https://bugs.winehq.org/show_bug.cgi?id=60397) and [60417](https://bugs.winehq.org/show_bug.cgi?id=60417); Proton issue [#9908](https://github.com/ValveSoftware/Proton/issues/9908) for FSR 4 |

What is carried
---------------

Two groups, kept apart on purpose: my patches for upstream Wine, and Proton-EM's FSR 4 work carried as it is.
[`docs-je/`](docs-je) has the full write-up of each issue, with the test programs and the Windows comparison.

### For upstream Wine: commits 1 to 5

Fixes for two bugs in Wine's synchronization primitives, found while tracking down the mid-mission crash of
Warhammer 40,000: Space Marine 2 under Proton ([#8072](https://github.com/ValveSoftware/Proton/issues/8072)).
They are four self-contained pieces, and each has its own page under [`docs-je/patches/`](docs-je/patches) with
the commit, the patch file, what it fixes, what it needs, where it applies, how to check it and where it stands
upstream:

| Patch | What it does | Upstream |
|---|---|---|
| [1](docs-je/patches/waitonaddress-use-after-return.md) | `RtlWakeAddressSingle()` and `RtlWakeAddressAll()` no longer touch a waiter's stack entry after clearing its address: a use-after-return on the path every lock and condition-variable wake takes | Wine bug [60397](https://bugs.winehq.org/show_bug.cgi?id=60397), point 1; the same change is pending as [wine-cachyos PR #34](https://github.com/CachyOS/wine-cachyos/pull/34) |
| [2 and 3](docs-je/patches/waitonaddress-stale-alert.md) | a `RtlWaitOnAddress()` waiter that times out just as a waker dequeues it takes the alert that is on its way, as Windows does, instead of leaving it to end the thread's next wait early; plus a conformance test. This is the Space Marine 2 crash | Wine bug 60397, points 2 and 3 |
| [4](docs-je/patches/ntsync-mutex-owned-after-close.md) | `wineserver` keeps an ntsync mutex that is owned when its last handle is closed, so that its owner's death still abandons it; the two `kernel32:sync` `test_mutex` failures | Wine bug [60417](https://bugs.winehq.org/show_bug.cgi?id=60417) |
| [5](docs-je/patches/ntsync-mutex-in-use-after-close.md) | the remaining case, a mutex closed while a wait-all of the same process is still pending on it; a new server request, so the protocol version changes | Wine bug 60417, second part |

All five apply with `git am` to Valve's Wine for this Experimental and to wine-cachyos; 1 to 3 also to Wine
master, 4 and 5 need a hand port there (checked 2026-10-09, details on the pages). The
[compare view](https://github.com/jegglest/wine/compare/6d211aabd1d...34ee3b22fd5) shows exactly these commits;
`.patch` appended to it, or to any commit link, makes GitHub generate the series or that one commit as a
`git am` file. No patch files are kept here: the commits are the patches.

**Stale thread alerts from `RtlWaitOnAddress()`** (patches 1–3, Wine bug
[60397](https://bugs.winehq.org/show_bug.cgi?id=60397)). `RtlWakeAddressSingle()` and `RtlWakeAddressAll()`
take a waiter off the list under a lock but alert its thread only after unlocking. A waiter whose timeout
expires in that window returns `STATUS_TIMEOUT` and leaves the alert pending, so the thread's next wait returns
immediately, whatever it is waiting for: the same mechanism is behind `SleepConditionVariableCS()`, SRW locks
and critical sections. Windows consumes the wake instead (measured on Windows 11). Patch 2 makes the waiter take
the in-flight alert, as Windows does; patch 1 fixes a use-after-return in the wakers, which kept touching the
waiter's stack entry after releasing it; patch 3 adds a conformance test that fails on stock Wine and passes on
Windows. In Space Marine 2 a spurious condition-variable wake lets a file-loader callback write into a stack
frame that is already gone, which is the `EXCEPTION_ACCESS_VIOLATION` in `resFILE_LOADER` reported in #8072.
The crash became frequent with Linux 7.2, which changed the timing, but the Wine bug is kernel-independent.

**ntsync mutexes that are never abandoned** (patches 4–5, Wine bug
[60417](https://bugs.winehq.org/show_bug.cgi?id=60417)). With ntsync, a mutex whose handles are all closed
while a thread still owns it, or is still using it, drops out of the server's bookkeeping, so when that thread
dies nobody waiting on the mutex gets `WAIT_ABANDONED`: they wait forever. Windows abandons it. Patch 4 keeps
such mutexes reachable until they are released or their owner dies; patch 5 covers a mutex closed while another
thread of the same process is still using it, which needs a new server request (the server protocol version is
bumped, so every unix-side library must come from the same tree, as it does in a full build like this one).
These were the two long-standing `kernel32:sync` test failures under ntsync.

Results so far: on my machine the stock build crashed regularly in Space Marine 2; with the patched `ntdll` and
`wineserver` it has not crashed since 2026-09-26 ([details](docs-je/stale-thread-alert.md#in-the-game)). Several
people in that thread report the same with Jpokul's prototype build, which carries the same patches. Wine's
`ntdll:sync` and `kernel32:sync` test suites pass with the patches, and so does the new test on Windows 11.

The synchronization patches were written with the help of an LLM (Claude), which Wine's contribution policy
excludes from merge requests, so upstream has them as bug reports with the analysis, and the patches live here
and on the Proton issue as a reference.

### Carried from Proton-EM, not for upstream: commits 6 to 31

These are Etaash Mathamsetty's commits, carried unchanged with their authorship so that the builds are nicer to
test with on RDNA4 cards. Nothing here tries to upstream them, and `amdxc64` is a Proton-only module in any case.

**FSR 4 on RDNA4 GPUs** (commits 6–31, Proton issue
[#9908](https://github.com/ValveSoftware/Proton/issues/9908)). A game with FSR 3.1 asks the AMD driver, through
`amdxc64.dll`, to swap in FSR 4. Proton Experimental has a minimal `amdxc64` in Wine and, in Steam's own build,
AMD's FSR 4 DLL (`contrib/amdxcffx64.dll`, FSR 4.1.1). The stub answers none of the questions the DLL asks before
choosing a model (the adapter family from `D3DKMTQueryAdapterInfo`, the wave-matrix properties, which shader
intrinsics work), so on RDNA4 the DLL announces FSR 4 but renders the FSR 3 model, and Valve limited the automatic
upgrade to RDNA3 to avoid that confusion (see the issue). Etaash Mathamsetty's
[Proton-EM](https://github.com/Etaash-mathamsetty/Proton) (Wine branch
[`em-11`](https://github.com/Etaash-mathamsetty/wine-valve/tree/em-11)) implements the driver side: `amdxc64` on AMD's SDK
interfaces, the `IAmdExtD3DDevice8` and shader-intrinsics queries answered from vkd3d-proton's wave-matrix
support, FSR 4.1.1's `UpdateFfxApiProviderEx`, the ML frame-generation upgrade, and `win32u` reporting the Navi4x
(FP8) or Navi3x (INT8) adapter family. Proton-GE ships this as patches in its EM-11 set; commits 6 to 31 are those
26 commits (the amdxc, win32u and d3dkmt patches of
[proton-ge-custom at `dceec5e940af`](https://github.com/GloriousEggroll/proton-ge-custom/tree/dceec5e940afd299f40304f24116b141d0798a72/patches/wine-hotfixes/wine-wayland)),
applied unchanged with their authorship. A build from source has no `contrib/`, so the `proton` script takes
Valve's `amdxcffx64.dll` from Steam's Proton Experimental or Proton Hotfix install, the same FSR 4.1.1 file
Proton-GE downloads, and copies it into the prefix as Valve's script does.

With the DLL available, the upgrade is automatic on discrete AMD GPUs from RDNA2 on, as in Proton-EM and
Proton-GE. `PROTON_FSR4_UPGRADE=0` turns it off, `PROTON_FSR4_UPGRADE=1` forces it on hardware it skips, such as
integrated GPUs, `PROTON_FSR4_INDICATOR=1` shows AMD's FSR watermark and `MLFG_UPGRADE=0` leaves frame generation
alone. RADV must expose `VK_KHR_cooperative_matrix`, and `VK_EXT_shader_float8` for the FP8 model on RDNA4
(`vulkaninfo | grep -E 'cooperative_matrix|shader_float8'`); vkd3d-proton as pinned by Valve already supports the
AMD wave-matrix intrinsics. Proton Experimental must be installed in Steam, which it is by default, or the DLL
placed in the tool's `contrib/` directory. On an RX 9070 XT the driver queries the FSR 4 DLL makes now come back
with FP8 wave-matrix support, where Valve's stub refuses them, and Helldivers 2 offers FSR 4 where it offered
FSR 3 before; the [write-up](docs-je/fsr4-rdna4.md) has the measurements.

### Not a patch

**AMD Anti-Lag 2** (nothing carried here, one variable to set). Games that integrate AMD's Anti-Lag 2 SDK,
Helldivers 2 among them, ask `amdxc64` for its `IAmdExtAntiLagApi` interface, which vkd3d-proton implements on
top of `VK_AMD_anti_lag`. RADV does not expose that extension itself: Mesa ships it as the Vulkan layer
`VK_LAYER_MESA_anti_lag`, which stays inactive unless `ENABLE_LAYER_MESA_ANTI_LAG=1` is in the game's
environment, so by default the query fails and the game's Anti-Lag 2 setting does nothing, with this build as
with Proton Experimental. Put `ENABLE_LAYER_MESA_ANTI_LAG=1 %command%` in the game's launch options and the
setting works; the variable is the same for every Proton, so this repository documents it rather than setting
it. Anti-Lag 1, the driver-only mode of the Windows driver, has no counterpart on Linux; DXVK's
`dxvk.latencySleep` option is the nearest thing for Direct3D 9 to 11 games. The
[write-up](docs-je/amd-anti-lag-2.md) has the measurements.

Installing a build
------------------

Download `proton-je-<version>.tar.xz` from [Releases](https://github.com/jegglest/proton-je/releases) and extract it
into `~/.steam/root/compatibilitytools.d/` (for Flatpak Steam,
`~/.var/app/com.valvesoftware.Steam/data/Steam/compatibilitytools.d/`):

```bash
sha512sum -c proton-je-<version>.sha512sum
tar -xf proton-je-<version>.tar.xz -C ~/.steam/root/compatibilitytools.d/
```

Restart Steam, then pick `proton-je-<version>` under the game's Properties > Compatibility. Tools such as
ProtonPlus or ProtonUp-Qt can do the same once they list this repository.

Building it yourself
--------------------

The build needs Podman or Docker, about 40 GB of disk and an hour or two, and otherwise works like upstream's
(see "Building Proton" below):

```bash
git clone --branch je-11.0 https://github.com/jegglest/proton-je
cd proton-je
git submodule update --init --recursive --filter=tree:0
mkdir build && cd build
../configure.sh --build-name=proton-je-local --enable-ccache
make redist          # the tool ends up in build/redist/
```

How this repository is laid out
-------------------------------

`je-11.0` is `experimental_11.0` plus a few commits: the `wine` submodule pointed at the `je-11.0` branch of
[`jegglest/wine`](https://github.com/jegglest/wine) (the Wine commit this Experimental uses plus the patches as
commits), the write-ups in `docs-je/`, the build workflow and this README. When Valve updates Experimental, the
Wine branch is rebased onto the Wine commit the new head uses and this branch onto the new head, and both are
force-pushed; every release stays reachable through its tag, which also pins the Wine commit it was built from.
Releases are tags named `proton-je-<Proton version>-<n>`, where `n` counts the builds on the same base.

Upstream's README follows.

---

Introduction
------------

**Proton** is a tool for use with the Steam client which allows games which are
exclusive to Windows to run on the Linux operating system. It uses Wine to
facilitate this.

**Most users should use Proton provided by the Steam Client itself.** See
[this Steam Community post][steam-play-introduction] for more details.

The source code is provided to enable advanced users the ability to alter
Proton. For example, some users may wish to use a different version of Wine
with a particular title.

**The changelog** is available on [our wiki][changelog].

[steam-play-introduction]: https://steamcommunity.com/games/221410/announcements/detail/1696055855739350561
[changelog]: https://github.com/ValveSoftware/Proton/wiki/Changelog


Obtaining Proton sources
------------------------

Acquire Proton's source by cloning <https://github.com/ValveSoftware/Proton>
and checking out the branch you desire.

You can clone the latest Proton to your system with this command:

```bash
git clone --recurse-submodules https://github.com/ValveSoftware/Proton.git proton
```

Be sure to update submodules when switching between branches:

```bash
git checkout experimental_6.3
git submodule update --init --recursive
```

If you want to change any subcomponent, now is the time to do so. For
example, if you wish to make changes to Wine, you would apply them to the
`wine/` directory.


Building Proton
---------------

Most of Proton builds inside the [Proton SDK](docker/README.md) (a downstream
of [SteamRT SDK][steamrt-sdk]) container with very few dependencies on the host
side.

## Preparing the build environment

You need either a Docker or a Podman setup which Proton's build system uses
internally. You should never need to use either container engine manually unless
working on those parts of build system directly.

We highly recommend [the rootless Podman setup][rootless-podman]. Please refer
to your distribution's documentation for setup instructions (e.g. Arch
[Podman][arch-podman] / [Docker][arch-docker], Debian [Podman][debian-podman] /
[Docker][debian-docker]).

[steamrt-sdk]: https://gitlab.steamos.cloud/steamrt/steamrt4/sdk
[rootless-podman]: https://github.com/containers/podman/blob/main/docs/tutorials/rootless_tutorial.md
[arch-podman]: https://wiki.archlinux.org/title/Podman
[arch-docker]: https://wiki.archlinux.org/title/Docker
[debian-podman]: https://wiki.debian.org/Podman
[debian-docker]: https://wiki.debian.org/Docker


## The Easy Way

We provide a top-level Makefile which will execute most of the build commands
for you.

After checking out the repository and updating its submodules, assuming that
you have a working Docker or Podman setup, you can build and install Proton
with a simple:

```bash
make install
```

If your build system is missing dependencies, it will fail quickly with a clear
error message.

After the build finishes, you may need to restart the Steam client to see the
new Proton tool. The tool's name in the Steam client will be based on the
currently checked out branch of Proton. You can override this name using the
`build_name` variable.

See `make help` for other build targets and options.



## Manual building

### Configuring the build

```bash
mkdir ../build && cd ../build
../proton/configure.sh --enable-ccache --build-name=my_build
```

Running `configure.sh` will create a `Makefile` allowing you to build Proton.
The scripts checks if containers are functional and prompt you if any
host-side dependencies are missing. You should run the command from a
directory created specifically for your build.

The configuration script tries to discover a working Docker or Podman setup
to use, but you can force a compatible engine with
`--container-engine=<executable_name>`.

You can enable ccache with `--enable-cache` flag. This will mount your
`$CCACHE_DIR` or `$HOME/.ccache` inside the container.

`--proton-sdk-image=registry.gitlab.steamos.cloud/proton/soldier/sdk:<version>`
can be used to build with a custom version of the Proton SDK images.

Check `--help` for other configuration options.

NOTE: If **SELinux** is in use, the Proton build container may fail to access
your user's files. This is caused by [SELinux's filesystem
labels][selinux-labels]. You may pass the `--relabel-volumes` switch to
configure to cause the [container engine to relabel its
bind-mounts][bind-mounts] and allow access to those files from within the
container. This can be dangerous when used with system directories. Proceed
with caution and refer your container engine's manual.

[selinux-labels]: https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/6/html/security-enhanced_linux/sect-security-enhanced_linux-working_with_selinux-selinux_contexts_labeling_files
[bind-mounts]: https://docs.docker.com/storage/bind-mounts/


### Building

```
make
```

**Important make targets:**

`make install` - install Proton into your user's Steam directory, see the [install Proton
locally](#install-proton-locally) section for details.

`make redist` - create a redistribute build (`redist/`) that can be copied to
`~/.steam/root/compatibilitytools.d/`.

`make deploy` - create a deployment build (`deploy/`). This is what we use to
deploy Proton to Steam users via Steamworks.

`make module=<module> module` - build both 32- and 64-bit versions of the
specified wine module. This allows rapid iteration on one module. This target
is only useful after building Proton.

`make dxvk` / `make vkd3d-proton` - rebuild DXVK / vkd3d-proton.


### Figuring Out What Failed To Build

Proton build system invokes builds of many subprojects in parallel. If one
subprojects fails there can be thousands of lines printed by other sub-builds
before the top level exits. This can make the real reason of the build failing
hard to find.

Appending `2>&1 | tee build.log` will log the full build output to a `build.log`
file. Searching that file from the bottom up for occurrences of `Error` should
point to the right area. E.g.:

```
make 2>&1 | tee build.log
grep -n '] Error [0-9]' build.log
```

```
11220:make: *** [../Makefile.in:465: /builds/proton/proton/build-dir/.kaldi-i386-configure] Error 1
12427:make: *** [../Makefile.in:1323: deploy] Error 2
```


### Debug Builds

To prevent symbol stripping add `UNSTRIPPED_BUILD=1` to the `make`
invocation. This should be used only with a clean build directory.

E.g.:

```
mkdir ../debug-proton-build && cd ../debug-proton-build
../proton/configure.sh --enable-ccache --build-name=debug_build
make UNSTRIPPED_BUILD=1 install
```


### ARM64 Builds

You need an ARM64 build machine and pass `--target-arch=arm64` to `configure.sh`.

It's not possible to use the resulting builds in x86 Steam running via FEX.


Install Proton locally
----------------------

Steam ships with several versions of Proton, which games will use by default or
that you can select in Steam Settings' Steam Play page. Steam also supports
running games with local builds of Proton, which you can install on your
machine.

To install a local build of Proton into Steam, make a new directory in
`~/.steam/root/compatibilitytools.d/` with a tool name of your choosing and
place the directory containing your redistributable build under that path.

The `make install` target will perform this task for you, installing the
Proton build into the Steam folder for the current user. You will have to
restart the Steam client for it to pick up on a new tool.

A correct local tool installation should look similar to this:

```
compatibilitytools.d/my_proton/
├── compatibilitytool.vdf
├── filelock.py
├── LICENSE
├── proton
├── proton_dist.tar
├── toolmanifest.vdf
├── user_settings.sample.py
└── version
```

To enable your local build in Steam, go to the Steam Play section of the
Settings window. If the build was correctly installed, you should see
"proton-localbuild" in the drop-down list of compatibility tools.

Each component of this software is used under the terms of their licenses.
See the `LICENSE` files here, as well as the `LICENSE`, `COPYING`, etc files
in each submodule and directory for details. If you distribute a built
version of Proton to other users, you must adhere to the terms of these
licenses.


Debugging
---------

Proton builds have their symbols stripped by default. You can switch to
"debug" beta branch in Steam (search for Proton in your library,
Properties... -> BETAS -> select "debug") or build without stripping (see
[Debug Builds section](#debug-builds)).

The symbols are provided through the accompanying `.debug` files which may
need to be explicitly loaded by the debugging tools. For GDB there's a helper
script `wine/tools/gdbinit.py` (source it) that provides `load-symbol-files`
(or `lsf` for short) command which loads the symbols for all the mapped files.

For tips on debugging see [docs/DEBUGGING-LINUX.md](docs/DEBUGGING-LINUX.md)
and [docs/DEBUGGING-WINDOWS.md](docs/DEBUGGING-WINDOWS.md).


`compile_commands.json`
-----------------------

For use with [clangd](https://clangd.llvm.org/) LSP server and similar tooling.

Projects built using cmake or meson (e.g. vkd3d-proton) automatically come with
`compile_commands.json`. Wine also generates the file on its own via `makedep`.

Proton's build system collects all the `compile_commands.json` files in a build
subdirectory named `compile_commands/`.

The paths are translated to point to the real source (i.e. not the rsynced
copy). It still may depend on build directory for things like auto-generated
`config.h` though and for wine it may be beneficial to run `tools/make_requests`
in you source directories as those changes are not committed.

You can then configure your editor to use that file for clangd in a few ways:

1) directly - some editors/plugins allow you to specify the path to `compile_commands.json`
2) via `.clangd` file, e.g.
```bash
cd src/proton/wine/
cat > .clangd <<EOF
CompileFlags:
  CompilationDatabase: ../build/current-dev/compile_commands/wine-x86_64/
EOF
```
3) by symlinking:
```bash
ln -s ../build/current-dev/compile_commands/wine-x86_64/compile_commands.json .
```


Runtime Config Options
----------------------

Proton can be tuned at runtime to help certain games run. The Steam client sets
some options for known games using the `STEAM_COMPAT_CONFIG` variable.
You can override these options using the environment variables described below.

The best way to set these environment overrides for all games is by renaming
`user_settings.sample.py` to `user_settings.py` and modifying it appropriately.
This file is located in the Proton installation directory in your Steam library
(often `~/.steam/steam/steamapps/common/Proton #.#`).

If you want to change the runtime configuration for a specific game, you can
use the `Set Launch Options` setting in the game's `Properties` dialog in the
Steam client. Set the variable, followed by `%command%`. For example, input
"`PROTON_USE_WINED3D=1 %command%`" to use the OpenGL-based wined3d renderer
instead of the Vulkan-based DXVK renderer.

To enable an option, set the variable to a non-`0` value.  To disable an
option, set the variable to `0`. To use Steam's default configuration, do
not specify the variable at all.

All of the below are runtime options. They do not effect permanent changes to
the Wine prefix. Removing the option will revert to the previous behavior.

| Compat config string  | Environment Variable               | Description  |
| :-------------------- | :--------------------------------- | :----------- |
|                       | `PROTON_LOG`                       | Convenience method for dumping a useful debug log to `$PROTON_LOG_DIR/steam-$APPID.log`. Set to `1` to enable default logging, or set to a string to be appended to the default `WINEDEBUG` channels. |
|                       | `PROTON_LOG_DIR`                   | Output log files into the directory specified. Defaults to your home directory. |
|                       | `PROTON_WAIT_ATTACH`               | Wait for a debugger to attach to steam.exe before launching the game process. To attach to the game process at startup, debuggers should be set to follow child processes. |
|                       | `PROTON_CRASH_REPORT_DIR`          | Write crash logs into this directory. Does not clean up old logs, so may eat all your disk space eventually. |
| `wined3d`             | `PROTON_USE_WINED3D`               | Use OpenGL-based wined3d instead of Vulkan-based DXVK for d3d11, d3d10, and d3d9. |
| `nod3d11`             | `PROTON_NO_D3D11`                  | Disable `d3d11.dll`, for d3d11 games which can fall back to and run better with d3d9. |
| `nod3d10`             | `PROTON_NO_D3D10`                  | Disable `d3d10.dll` and `dxgi.dll`, for d3d10 games which can fall back to and run better with d3d9. |
| `dxvkd3d8`            | `PROTON_DXVK_D3D8`                 | Use DXVK's `d3d8.dll`. |
| `nofsync`             | `PROTON_NO_FSYNC`                  | Do not use futex-based in-process synchronization primitives. (Automatically disabled on systems with no `FUTEX_WAIT_MULTIPLE` support.) |
|                       | `PROTON_NO_NTSYNC`                 | Do not use ntsync. |
|                       | `HOST_LC_ALL`                      | Set value to a locale to override all other system locale settings for a game.  This variable should be used instead of `LC_ALL`. |
| `disablenvapi`        | `PROTON_DISABLE_NVAPI`             | Disable NVIDIA's NVAPI GPU support library. |
| `nativevulkanloader`  |                                    | Use the Vulkan loader shipped with the game instead of Proton's built-in Vulkan loader. This breaks VR support, but is required by a few games. |
| `forcelgadd`          | `PROTON_FORCE_LARGE_ADDRESS_AWARE` | Force Wine to enable the LARGE_ADDRESS_AWARE flag for all executables. Enabled by default. |
| `heapdelayfree`       | `PROTON_HEAP_DELAY_FREE`           | Delay freeing some memory, to work around application use-after-free bugs. |
| `gamedrive`           | `PROTON_SET_GAME_DRIVE`            | Create an S: drive which points to the Steam Library which contains the game. |
| `noforcelgadd`        |                                    | Disable forcelgadd. If both this and `forcelgadd` are set, enabled wins. |
| `oldglstr`            | `PROTON_OLD_GL_STRING`             | Set some driver overrides to limit the length of the GL extension string, for old games that crash on very long extension strings. |
| `vkd3dfl12`           |                                    | Force the Direct3D 12 feature level to 12, regardless of driver support. |
| `vkd3dbindlesstb`     |                                    | Put `force_bindless_texel_buffer` into `VKD3D_CONFIG`. |
| `nomfdxgiman`         | `WINE_DO_NOT_CREATE_DXGI_DEVICE_MANAGER` | Enable hack to work around video issues in some games due to incomplete IMFDXGIDeviceManager support. |
| `noopwr`              | `WINE_DISABLE_VULKAN_OPWR`               | Enable hack to disable Vulkan other process window rendering which sometimes causes issues on Wayland due to blit being one frame behind. |
| `hidenvgpu`           | `PROTON_HIDE_NVIDIA_GPU`           | Force Nvidia GPUs to always be reported as AMD GPUs. Some games require this if they depend on Windows-only Nvidia driver functionality. See also DXVK's nvapiHack config, which only affects reporting from Direct3D. |
|                       | `WINE_FULLSCREEN_INTEGER_SCALING`  | Enable integer scaling mode, to give sharp pixels when upscaling. |
|                       | `WINE_USE_KWIN_HACKS`              | Enable KDE-specific windowing hacks that may improve experience with KDE older than 6.4 on Wayland and KDE older than 6.6 on X11. |
| `cmdlineappend:`      |                                    | Append the string after the colon as an argument to the game command. May be specified more than once. Escape commas and backslashes with a backslash. |
| `xalia` or `noxalia`  | `PROTON_USE_XALIA`                 | Enable Xalia, a program that can add a gamepad UI for some keyboard/mouse interfaces, or set to 0 to disable. The default is to enable it dynamically based on window contents. |
| `fnad3d11`            | `FNA3D_FORCE_DRIVER=D3D11`         | Force FNA to use D3D11 for rendering. |
| `seccomp`             | `PROTON_USE_SECCOMP`               | **Note: Obsoleted in Proton 5.13.** In older versions, enable seccomp-bpf filter to emulate native syscalls, required for some DRM protections to work. |
| `d9vk`                | `PROTON_USE_D9VK`                  | **Note: Obsoleted in Proton 5.0.** In older versions, use Vulkan-based DXVK instead of OpenGL-based wined3d for d3d9. |
| `noesync`             | `PROTON_NO_ESYNC`                  | **Note: Obsoleted in Proton 11.0.** In older versions, do not use eventfd-based in-process synchronization primitives. |

<!-- Target:  GitHub Flavor Markdown.  To test locally:  pandoc -f markdown_github -t html README.md  -->
