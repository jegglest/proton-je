/* amd_probe: create a D3D12 device and ask the amdxc64 driver shim for the interfaces AMD's
 * Anti-Lag 2 SDK and FSR 4 use. Prints one line per step. */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdarg.h>

DEFINE_GUID(IID_IAmdExtFfxApi,            0xb58d6601,0x7401,0x4234,0x81,0x80,0x6f,0xeb,0xfc,0x0e,0x48,0x4c);
DEFINE_GUID(IID_IAmdExtAntiLagApi,        0x44085fbe,0xe839,0x40c5,0xbf,0x38,0x0e,0xbc,0x5a,0xb4,0xd0,0xa6);
DEFINE_GUID(IID_IAmdExtD3DFactory,        0x014937ec,0x9288,0x446f,0xa9,0xac,0xd7,0x5a,0x8e,0x3a,0x98,0x4f);
DEFINE_GUID(IID_IAmdExtD3DShaderIntrinsics,0xba019d53,0xccab,0x4cbd,0xb5,0x6a,0x72,0x30,0xed,0x43,0x30,0xad);
DEFINE_GUID(IID_IAmdExtD3DDevice8,        0xf714e11a,0xb54e,0x4e0f,0xab,0xc5,0xdf,0x58,0xb1,0x81,0x33,0xd1);

typedef HRESULT (__cdecl *PFN_AmdExtD3DCreateInterface)(IUnknown *outer, REFIID iid, void **out);
typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID iid, void **out);

/* vtable slots, counted from the amdxc64 IDL */
#define VT(obj) (*(void ***)(obj))
typedef HRESULT (STDMETHODCALLTYPE *pfn_QueryInterface)(void *, REFIID, void **);
typedef ULONG   (STDMETHODCALLTYPE *pfn_Release)(void *);
typedef HRESULT (STDMETHODCALLTYPE *pfn_UpdateAntiLagState)(void *, void *);               /* IAmdExtAntiLagApi slot 3 */
typedef HRESULT (STDMETHODCALLTYPE *pfn_CreateInterface)(void *, IUnknown *, REFIID, void **); /* IAmdExtD3DFactory slot 3 */
typedef HRESULT (STDMETHODCALLTYPE *pfn_CheckSupport)(void *, UINT);                      /* IAmdExtD3DShaderIntrinsics slot 4 */
typedef HRESULT (STDMETHODCALLTYPE *pfn_GetWaveMatrixProperties)(void *, SIZE_T *, void *); /* IAmdExtD3DDevice8 slot 16 */
typedef HRESULT (STDMETHODCALLTYPE *pfn_UpdateFfxApiProvider)(void *, void *, unsigned int); /* IAmdExtFfxApi slot 3 */

struct antilag_v1 { unsigned int uiSize, uiVersion, eMode; const char *sControlStr; unsigned int uiControlStrLength, maxFPS; };
struct wmma_props { SIZE_T m, n, k; ULONG a, b, c, result, saturating; };
struct ffx_provider { ULONG structVersion; ULONG64 descType, versionId; const char *versionName; void *fn[6]; };

static FILE *out_file;
static int out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (out_file) { va_start(ap, fmt); vfprintf(out_file, fmt, ap); va_end(ap); fflush(out_file); }
    fflush(stdout);
    return 0;
}
#define printf out

int main(int argc, char **argv)
{
    HMODULE d3d12 = LoadLibraryA("d3d12.dll"), amdxc;
    setvbuf(stdout, NULL, _IONBF, 0);
    out_file = fopen("amd_probe_out.txt", "w");
    PFN_D3D12CreateDevice create;
    PFN_AmdExtD3DCreateInterface create_iface;
    ID3D12Device *device = NULL;
    void *antilag = NULL, *factory = NULL, *intrinsics = NULL, *device8 = NULL, *ffx = NULL;
    HRESULT hr;
    int ffx_test = argc > 1 && !strcmp(argv[1], "--ffx");

    if (!d3d12 || !(create = (void *)GetProcAddress(d3d12, "D3D12CreateDevice"))) { printf("d3d12.dll: not usable\n"); return 1; }
    hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device);
    printf("D3D12CreateDevice: %#lx\n", hr);
    if (FAILED(hr)) return 1;

    amdxc = GetModuleHandleA("amdxc64.dll");
    printf("amdxc64.dll loaded after device creation (what the Anti-Lag 2 SDK requires): %s\n", amdxc ? "yes" : "NO");
    if (!amdxc) { amdxc = LoadLibraryA("amdxc64.dll"); printf("LoadLibrary(amdxc64.dll): %s\n", amdxc ? "ok" : "failed"); }
    if (!amdxc || !(create_iface = (void *)GetProcAddress(amdxc, "AmdExtD3DCreateInterface"))) { printf("AmdExtD3DCreateInterface: missing\n"); return 1; }

    /* Anti-Lag 2, exactly the SDK's Initialize(): disabled during init, then enabled */
    hr = create_iface((IUnknown *)device, &IID_IAmdExtAntiLagApi, &antilag);
    printf("IAmdExtAntiLagApi: %#lx%s\n", hr, hr == S_OK ? " (Anti-Lag 2 available)" : " (Anti-Lag 2 NOT available)");
    if (hr == S_OK)
    {
        struct antilag_v1 data = { sizeof(data), 1, 2, NULL, 0, 0 };
        hr = ((pfn_UpdateAntiLagState)VT(antilag)[3])(antilag, &data);
        printf("  UpdateAntiLagState(mode 2 = off): %#lx\n", hr);
        data.eMode = 1;
        hr = ((pfn_UpdateAntiLagState)VT(antilag)[3])(antilag, &data);
        printf("  UpdateAntiLagState(mode 1 = on): %#lx\n", hr);
        hr = ((pfn_UpdateAntiLagState)VT(antilag)[3])(antilag, NULL);
        printf("  UpdateAntiLagState(NULL = frame marker): %#lx\n", hr);
    }

    /* FSR 4 / AGS driver extension interfaces */
    hr = create_iface((IUnknown *)device, &IID_IAmdExtD3DFactory, &factory);
    printf("IAmdExtD3DFactory: %#lx\n", hr);
    if (hr == S_OK)
    {
        hr = ((pfn_CreateInterface)VT(factory)[3])(factory, (IUnknown *)device, &IID_IAmdExtD3DShaderIntrinsics, &intrinsics);
        printf("  IAmdExtD3DShaderIntrinsics: %#lx\n", hr);
        if (hr == S_OK)
        {
            printf("    CheckSupport(WaveMatrix): %#lx\n", ((pfn_CheckSupport)VT(intrinsics)[4])(intrinsics, 0x1F));
            printf("    CheckSupport(Float8Conversion): %#lx\n", ((pfn_CheckSupport)VT(intrinsics)[4])(intrinsics, 0x20));
            printf("    CheckSupport(Readfirstlane): %#lx\n", ((pfn_CheckSupport)VT(intrinsics)[4])(intrinsics, 0x1));
        }
        hr = ((pfn_CreateInterface)VT(factory)[3])(factory, (IUnknown *)device, &IID_IAmdExtD3DDevice8, &device8);
        printf("  IAmdExtD3DDevice8: %#lx\n", hr);
        if (hr == S_OK)
        {
            struct wmma_props props[8] = {0};
            SIZE_T count = 8;
            hr = ((pfn_GetWaveMatrixProperties)VT(device8)[16])(device8, &count, props);
            printf("    GetWaveMatrixProperties: %#lx, %u entries\n", hr, (unsigned)count);
            for (SIZE_T i = 0; hr == S_OK && i < count; i++)
                printf("      %ux%ux%u a=%lu b=%lu c=%lu result=%lu (11 = FP8)\n", (unsigned)props[i].m, (unsigned)props[i].n,
                       (unsigned)props[i].k, props[i].a, props[i].b, props[i].c, props[i].result);
        }
    }

    /* The FSR 3.1 -> FSR 4 provider upgrade itself; only on request since it loads amdxcffx64.dll */
    hr = create_iface((IUnknown *)device, &IID_IAmdExtFfxApi, &ffx);
    printf("IAmdExtFfxApi: %#lx\n", hr);
    if (hr == S_OK && ffx_test)
    {
        struct ffx_provider provider = {0};
        hr = ((pfn_UpdateFfxApiProvider)VT(ffx)[3])(ffx, &provider, sizeof(provider));
        printf("  UpdateFfxApiProvider: %#lx, provider version %s (id %#I64x)\n", hr,
               provider.versionName ? provider.versionName : "(none)", provider.versionId);
    }

    if (antilag) ((pfn_Release)VT(antilag)[2])(antilag);
    ID3D12Device_Release(device);
    printf("done\n");
    return 0;
}
