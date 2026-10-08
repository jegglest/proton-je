# FSR 4 renders the FSR 3 model on RDNA4

| | |
|---|---|
| Patches | commits 6 to 31 on [`jegglest/wine`, branch `je-11.0`](https://github.com/jegglest/wine/commits/je-11.0) (`dlls/amdxc64/`, `dlls/win32u/d3dkmt.c`), Etaash Mathamsetty's work from Proton-EM as Proton-GE ships it, plus the `proton` script change in this repository |
| Upstream | Proton issue [#9908](https://github.com/ValveSoftware/Proton/issues/9908), open since 2026-06-24; the Proton-EM commits are not in Valve's Wine |
| Seen as | Proton Experimental on an RDNA4 card (RX 9070, 9070 XT, 9060 XT): games with FSR 3.1 stay on FSR 3.1, and with `FSR4_UPGRADE=1` they say FSR 4 but render the FSR 3 model |
| Affects | Proton Experimental and Bleeding Edge since Valve's `amdxc64` auto-enable commit of 2026-06-21; every build of Proton from source also lacks the FSR 4 DLL |
| Windows | AMD's driver provides the upgrade on RDNA3 and RDNA4; the user switches it on per game in Adrenalin |

## Summary

A game built with FSR 3.1 asks the AMD driver, through `amdxc64.dll`, to replace its upscaler with FSR 4. Proton
Experimental ships a minimal `amdxc64` in Wine and, in the build Steam installs, AMD's FSR 4 DLL
(`contrib/amdxcffx64.dll`, FSR 4.1.1). Before the DLL picks a model it asks the driver what the GPU can do; the
stub answers none of those questions, so on RDNA4 the DLL falls back to the FSR 3 model while reporting FSR 4.
Valve therefore enables the upgrade automatically on RDNA3 only (a device-ID list) and left RDNA4 out on purpose.
Proton-EM implements the driver side properly and Proton-GE ships it; this branch carries those 26 commits
unchanged. On the Proton side, a build from source has no `contrib/`, so the `proton` script takes Valve's copy of
the DLL from Steam's Proton Experimental or Proton Hotfix install.

## Symptom

On an RDNA4 card, games keep FSR 3.1 under Proton Experimental. Helldivers 2 names the FSR version in its video
settings and shows FSR 3; the reporters in #9908 list Crimson Desert, Kingdom Come: Deliverance II, Titan Quest 2,
Manor Lords, Final Fantasy VII Rebirth and Marvel's Spider-Man 2. The same card with Proton-GE, Proton-CachyOS
or OptiScaler gets FSR 4, and an RX 7600 (RDNA3) gets it from Proton Experimental itself. Forcing the upgrade
with `FSR4_UPGRADE=1` is worse: the game then reports FSR 4 while rendering the FSR 3 model, which is the
confusion Valve's collaborator describes in the issue.

## The Wine side

The upgrade path, as the FidelityFX SDK uses it: the game's `amd_fidelityfx_dx12.dll` (or the statically linked
FFX API) loads `amdxc64.dll`, calls `AmdExtD3DCreateInterface()` for `IAmdExtFfxApi` and
`UpdateFfxApiProvider()`; the driver's `amdxc64` loads `amdxcffx64.dll`, AMD's FSR 4 implementation, which
registers itself as a provider for the game's FSR contexts. Before it does, the DLL asks the driver which model
it can run:

- `D3DKMTQueryAdapterInfo(KMTQAITYPE_UMDRIVERPRIVATE)`, AMD's private adapter block (0x4360 bytes), whose
  adapter family selects the Navi4x FP8 model, the Navi3x INT8 model, or nothing;
- `IAmdExtD3DFactory::CreateInterface()` for `IAmdExtD3DDevice8`, whose `GetWaveMatrixProperties()` lists the
  wave-matrix (WMMA) types the GPU supports;
- `IAmdExtD3DShaderIntrinsics::CheckSupport()` for `WaveMatrix` and `Float8Conversion`.

Valve's `amdxc64` ([`2c236b9a0ba`](https://github.com/ValveSoftware/wine/commit/2c236b9a0baee7730b2da1b246a6bd04089a1e8f),
Etaash Mathamsetty's initial DLL from July 2025, and
[`77f9f14ad59`](https://github.com/ValveSoftware/wine/commit/77f9f14ad5996e22151f63acc9de492afde356b4), the
auto-enable of June 2026) implements `IAmdExtFfxApi` with the legacy `UpdateFfxApiProvider()` only, returns
`E_NOINTERFACE` for the factory, and `win32u`'s `NtGdiDdDDIQueryAdapterInfo()` has no `KMTQAITYPE_UMDRIVERPRIVATE`
case, so the DLL gets `STATUS_NOT_IMPLEMENTED` and settles for the FSR 3 model. The auto-enable list,
`0x73f0` and `0x7440` to `0x749f`, is Navi 31, 32 and 33; Navi 48 (`0x7550`, `0x7551`) and Navi 44 (`0x7590`) are
not in it.

## What Windows does

AMD's driver answers all three queries, and the FSR 4.1.1 DLL runs the FP8 model on RDNA4 and the INT8 model on
RDNA3. The upgrade is off by default and switched on per game in Adrenalin's driver settings, which is the step
`FSR4_UPGRADE` or the automatic enable replaces under Proton.

## The fix

[Proton-EM](https://github.com/Etaash-mathamsetty/Proton) (Etaash Mathamsetty) implements the driver side on top of
Valve's stub, and Proton-GE applies the commits as the amdxc, win32u and d3dkmt patches of its EM-11 set
([`patches/wine-hotfixes/wine-wayland/`](https://github.com/GloriousEggroll/proton-ge-custom/tree/dceec5e940afd299f40304f24116b141d0798a72/patches/wine-hotfixes/wine-wayland)
at `dceec5e9`). Commits 6 to 31 of the Wine branch are those 26 patches applied with `git am`, so author, date
and message are the originals; the GE patch numbers are in brackets:

- `amdxc64`: revert Valve's auto-enable [0001]; port to AMD's SDK interface headers [0035]; stub
  `IAmdExtD3DDevice8` [0036]; `FSR_WATERMARK` [0037]; WMMA support check [0038]; the ML frame-generation (MLFG)
  override, its user control and watermark, on by default [0039–0042, 0178]; query WMMA and FP8 support from
  vkd3d-proton's `ID3D12DeviceExt3::SupportsAGSExtension()` [0125]; quieter `IAmdExtD3DDevice1` [0133]; factory
  singleton [0144]; FSR 4.1.1's `UpdateFfxApiProviderEx()` [0147]; automatic upgrades [0148] on discrete RDNA2+
  hardware, checked through vkd3d-proton's Vulkan interop [0150]; drop the FSR 3 upgrade path [0171]; one-time
  FP16 fixme [0181]; `AmdExtD3DShaderIntrinsics::CheckSupport()` per intrinsic [0219]; `IAmdExtD3DCreateDevice`
  [0234].
- `win32u`: `KMTQAITYPE_UMDRIVERPRIVATE` [0146, 0149, 0165, 0172, 0225] reports the Navi4x family when RADV
  exposes `VK_EXT_shader_float8` and `VK_NV_cooperative_matrix2`, the Navi3x family on other discrete RDNA2+
  cards or with `FSR4_UPGRADE=1`, in the 0x4360-byte block that AGS reads; `KMTQAITYPE_UMDRIVERNAME` [0235]
  names `amdxc64.dll` as the D3D12 driver.

The series applied without conflicts on Valve's Wine `6d211aab` (Proton-GE's base is 58 commits earlier, with
no change to these files in between). Valve's `f18c248da74` (stub singleton, June 2026) is in both trees.
vkd3d-proton as Valve pins it (`44cf7c20`, 2026-09-30) implements `SupportsAGSExtension()` for the FP8 and
native-FP8 WMMA extensions, which is all the new code asks of it.

Not taken from Proton-GE: its `win32u: Implement NtGdiDdDDIQueryAdapterInfo cases` patch (other query types,
unrelated), the `kernelbase` DLL-override hack and the protonfixes downloader. GE's `PROTON_FSR4_UPGRADE`
downloads `amdxcffx64.dll` from the [proton-upscalers manifest](https://loathingkernel.github.io/proton-upscalers/manifest.json);
Steam's Proton Experimental and Proton Hotfix already carry the identical FSR 4.1.1 file (MD5
`398ea93c15d554efb7ece1f4cd057554`) in `contrib/`, so the `proton` script here looks for it in the Steam install
and the game's library (`find_amdxcffx64()`) when the tool's own `contrib/` has none, and copies it into the
prefix exactly as Valve's script does. Without a copy it logs
`amdxcffx64.dll not found, no FSR 4 upgrades` and the game keeps FSR 3.1.

| Setting | Effect |
|---|---|
| nothing | FSR 3.1 games get FSR 4 on discrete AMD GPUs from RDNA2 on when the DLL is available (Proton-EM's and Proton-GE's behaviour) |
| `PROTON_FSR4_UPGRADE=0` | no upgrade |
| `PROTON_FSR4_UPGRADE=1` | upgrade also where `amdxc64` would not enable it by itself, such as integrated GPUs |
| `PROTON_FSR4_INDICATOR=1` | AMD's FSR and frame-generation watermarks (`FSR_WATERMARK`, `FSR_FG_WATERMARK`) |
| `MLFG_UPGRADE=0` | keep the game's frame generation; by default the DLL's ML frame generation replaces it where FP8 is supported |

RADV must expose `VK_KHR_cooperative_matrix`, and `VK_EXT_shader_float8` for the FP8 model on RDNA4; both are
there on the test machine's Mesa 26.2.4.

## Testing

### Build

The two changed compilation units (`dlls/amdxc64/main.c` with its generated interface headers, and
`dlls/win32u/d3dkmt.c`) compile without warnings under Fedora 42's gcc 15 with Wine's `-Wall` set, and
`amdxc64.dll` links against `vulkan-1.dll` for the hardware check. The full tool comes from the CI build of the
tagged commit, as every release here.

### The driver interfaces

[`repro/amd_probe.c`](repro/amd_probe.c) creates a D3D12 device and asks `amdxc64` the questions the FSR 4 DLL
asks, on the RX 9070 XT with RADV (Mesa 26.2.4), through each tool's `proton` script with nothing set in the
environment:

| Query | `proton-je-11.0-20261001-1` (Valve's `amdxc64`) | dev build of commit `9910b516` (these patches) |
|---|---|---|
| `amdxcffx64.dll` in the prefix | no (`contrib/` missing; `try_copy` error in the log) | yes, 65,657,608 bytes, from Steam's Proton Experimental |
| `IAmdExtD3DFactory` | `S_OK`, but an unnamed stub | `S_OK` |
| `IAmdExtD3DShaderIntrinsics::CheckSupport(WaveMatrix)` | `0x4c` (stub returns garbage) | `S_OK` |
| `IAmdExtD3DShaderIntrinsics::CheckSupport(Float8Conversion)` | `0x4c` | `S_OK` |
| `IAmdExtD3DDevice8` | `0x80004002` (`E_NOINTERFACE`) | `S_OK` |
| `IAmdExtD3DDevice8::GetWaveMatrixProperties()` | not reachable | 1 entry: 16x16x16, A and B FP8 (type 11), C and result FP32 |
| `amdxc` trace | `get_luid`, device not in the Navi 3x list, no upgrade | `check_intrinsic_support FSR4 FP8 supported!`, `amdxcffx64` loaded and called through `UpdateFfxApiProviderEx()` |

The wave-matrix entry and the two `CheckSupport()` answers are what the FSR 4 DLL needs to choose the FP8 model;
Valve's stub cannot give them. The probe's own `UpdateFfxApiProvider()` call passes an empty request and gets
`E_INVALIDARG` back from the DLL, so the provider itself is exercised by the game, not here.

### In the game

Helldivers 2 on the test machine (RX 9070 XT, RADV, Mesa 26.2.4), which names the FSR version in its video
settings and ships the FSR 4 SDK (`amd_fidelityfx_upscaler_dx12.dll`), so it takes the same driver interfaces for
its native FSR 4 as the upgrade path does: with the dev build of commit `9910b516` the game offers FSR 4, where
Proton Experimental and the previous release offer FSR 3 only (2026-10-08).

## Upstream status

Valve's position in #9908 (2026): the automatic upgrade is limited to RDNA3 so that nobody sees FSR 4 in the
settings and FSR 3 on screen; the issue stays open. The Proton-EM commits exist outside Valve's tree only, in
Proton-EM, Proton-GE and Proton-CachyOS. `amdxc64` is a Proton-only module, so there is nothing to report to
Wine itself.

## References

- Proton issue [#9908](https://github.com/ValveSoftware/Proton/issues/9908), "Proton Experimental does not enable
  FSR4 on RDNA4 correctly", and the general FSR 4 tracker [#8519](https://github.com/ValveSoftware/Proton/issues/8519)
- Valve's `amdxc64`: [`2c236b9a0ba`](https://github.com/ValveSoftware/wine/commit/2c236b9a0baee7730b2da1b246a6bd04089a1e8f),
  [`f18c248da74`](https://github.com/ValveSoftware/wine/commit/f18c248da74), [`77f9f14ad59`](https://github.com/ValveSoftware/wine/commit/77f9f14ad5996e22151f63acc9de492afde356b4)
- Proton-EM: [Etaash-mathamsetty/Proton](https://github.com/Etaash-mathamsetty/Proton), Wine branch
  [`em-11` of Etaash-mathamsetty/wine-valve](https://github.com/Etaash-mathamsetty/wine-valve/tree/em-11)
- Proton-GE: [the EM-11 patch set](https://github.com/GloriousEggroll/proton-ge-custom/tree/dceec5e940afd299f40304f24116b141d0798a72/patches/wine-hotfixes/wine-wayland)
  and its [README on FSR 4](https://github.com/GloriousEggroll/proton-ge-custom#enabling-fsr-4)
- vkd3d-proton: [`SupportsAGSExtension()`](https://github.com/HansKristian-Work/vkd3d-proton/blob/44cf7c2042168f3b8ee37249f0a53ac091e48fd9/libs/vkd3d/device_vkd3d_ext.c)
