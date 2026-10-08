# AMD Anti-Lag 2 is refused under Proton

| | |
|---|---|
| Patches | none; put `ENABLE_LAYER_MESA_ANTI_LAG=1` in the game's launch options (documented here, not carried) |
| Upstream | Mesa ships `VK_AMD_anti_lag` as the layer `VK_LAYER_MESA_anti_lag`, off unless asked for; vkd3d-proton implements the Anti-Lag 2 interface on it; nothing to report |
| Seen as | games with AMD Anti-Lag 2 (Helldivers 2, Counter-Strike 2's Windows build, Ghost of Tsushima and others) offer the setting under Proton but it has no effect |
| Affects | every Proton on every Mesa with the layer (Mesa 25.1 and later), unless the environment enables the layer |
| Windows | the AMD driver's `amdxc64.dll` answers the SDK directly; the game's toggle works out of the box |

## Summary

AMD's Anti-Lag 2 SDK is a header the game compiles in. It finds the driver's `amdxc64.dll` with
`GetModuleHandle()` (the DLL has to be loaded already), asks it for `IAmdExtAntiLagApi` and calls
`UpdateAntiLagState()` once per frame. Under Proton everything on that path exists: vkd3d-proton loads
`amdxc64.dll` when it creates a D3D12 device on an AMD GPU, Wine's `amdxc64` forwards the interface query to the
device, and vkd3d-proton implements the interface with `VK_AMD_anti_lag`. The one missing piece is the Vulkan
extension: RADV does not expose it, Mesa provides it through an implicit layer that is inactive until
`ENABLE_LAYER_MESA_ANTI_LAG=1` is set. Without it vkd3d-proton returns `E_NOINTERFACE` and the game's setting is
dead. Setting the variable is all it takes, and it is the same for every Proton, so this repository documents it
rather than carrying a change.

## Symptom

The game shows its Anti-Lag 2 option (it only checks for an AMD GPU) but enabling it changes nothing, and nothing
in the logs says why. With `WINEDEBUG=+amdxc` the query shows up as
`AmdExtD3DCreateInterface ... iid {44085fbe-e839-40c5-bf38-0ebc5ab4d0a6}` and returns `E_NOINTERFACE`.

## The Linux side

- `ffx_antilag2_dx12.h` (the [Anti-Lag 2 SDK](https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK)):
  `GetModuleHandleA("amdxc64.dll")`, `AmdExtD3DCreateInterface(device, IID_IAmdExtAntiLagApi)`, then
  `UpdateAntiLagState()` with `eMode` 2 (off) during initialization, 1 (on) when the game enables it, and with a
  null pointer as the per-frame marker.
- vkd3d-proton (`d3d12_device_init_vendor_hacks()`) loads `amdxc64.dll` on AMD devices precisely so that the SDK
  can find it, implements `IAmdExtAntiLagApi` 1:1 (`UpdateAntiLagState()` becomes `vkAntiLagUpdateAMD()`), and
  hands the interface out only when the device reports the `antiLag` feature and no `VK_NV_low_latency2`.
- Wine's `amdxc64` (Valve's and Proton-EM's alike) answers `IID_IAmdExtAntiLagApi` by forwarding to the device's
  `QueryInterface()`.
- RADV on this machine (Mesa 26.2.4, RX 9070 XT): `VK_AMD_anti_lag` absent, `antiLag` false. Mesa's
  `/usr/share/vulkan/implicit_layer.d/VkLayer_MESA_anti_lag.json` adds the extension on any device, with
  `"enable_environment": {"ENABLE_LAYER_MESA_ANTI_LAG": "1"}` and `"disable_environment":
  {"DISABLE_LAYER_MESA_ANTI_LAG": "1"}`.

The D3D11 variant of the SDK (`ffx_antilag2_dx11.h`) looks for `amdxx64.dll` and `AmdDxExtCreate11()`, the AMD
Direct3D 11 driver, which neither Proton nor DXVK provides; Proton-EM's `atidxx64` stub logs
`D3D11 Anti-Lag 2 is not supported!`. Anti-Lag 1, the driver-side mode without game integration, has no Linux
counterpart; for Direct3D 9 to 11 games DXVK's `dxvk.latencySleep` option is the nearest thing (DXVK issue
[#4268](https://github.com/doitsujin/dxvk/issues/4268) tracks Anti-Lag 2 there).

## What Windows does

The AMD driver's own `amdxc64.dll` implements the interface, so the toggle works in every game that integrates
the SDK, on every GPU the driver supports.

## What to do

Put `ENABLE_LAYER_MESA_ANTI_LAG=1 %command%` in the game's launch options, or export the variable for the Steam
process. The layer only makes the extension available; whether anti-lag runs stays with the game's setting, and
nothing changes for games without the SDK, for DXVK, or on other GPUs (vkd3d-proton loads `amdxc64` on AMD
only). Mesa keeps the layer opt-in on purpose, and the switch works the same under every Proton, which is why
this build does not set it for you.

## Testing

[`repro/amd_probe.c`](repro/amd_probe.c) creates a D3D12 device and goes through the SDK's steps, plus the FSR 4
interface queries from the [FSR 4 write-up](fsr4-rdna4.md). Run through the tool's `proton` script so that the
prefix has vkd3d-proton's `d3d12.dll`; `repro/` has the commands. On the RX 9070 XT with Mesa 26.2.4 and the
Proton-JE release built before this change (`proton-je-11.0-20261001-1`, Valve's `amdxc64`):

| Environment | `VK_AMD_anti_lag` (direct query) | `IAmdExtAntiLagApi` | `UpdateAntiLagState()` off, on, frame marker |
|---|---|---|---|
| nothing set | absent, `antiLag` false | `0x80004002` (`E_NOINTERFACE`) | not reachable |
| `ENABLE_LAYER_MESA_ANTI_LAG=1` | listed, `antiLag` true | `S_OK` | `S_OK`, `S_OK`, `S_OK` |

The in-game check is Helldivers 2's Anti-Lag 2 setting, which the game greys out when the interface is missing.

## Upstream status

Nothing to report: Mesa keeps the layer opt-in on purpose (vkd3d-proton's source notes that anti-lag "will not
be enabled by default until it's confirmed to be rock solid"), vkd3d-proton has implemented the interface since
mid-2025 ([#2526](https://github.com/HansKristian-Work/vkd3d-proton/pull/2526), simplified in
[#3083](https://github.com/HansKristian-Work/vkd3d-proton/pull/3083)), and no Proton sets the variable: not
Valve's, not Proton-GE as of its current `master`, and not this one.

## References

- Anti-Lag 2 SDK: [`ffx_antilag2_dx12.h`](https://github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK/blob/main/ffx_antilag2_dx12.h)
- vkd3d-proton: [`d3d12_device_init_vendor_hacks()`](https://github.com/HansKristian-Work/vkd3d-proton/blob/44cf7c2042168f3b8ee37249f0a53ac091e48fd9/libs/vkd3d/device.c)
  and the anti-lag interface in [`device_vkd3d_ext.c`](https://github.com/HansKristian-Work/vkd3d-proton/blob/44cf7c2042168f3b8ee37249f0a53ac091e48fd9/libs/vkd3d/device_vkd3d_ext.c)
- Mesa: the `VK_LAYER_MESA_anti_lag` layer (`src/vulkan/anti-lag-layer` in the Mesa tree)
- DXVK issue [#4268](https://github.com/doitsujin/dxvk/issues/4268), Anti-Lag 2 for Direct3D 11
