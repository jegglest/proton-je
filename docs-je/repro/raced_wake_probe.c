/* Probe for a stale thread alert after a RtlWaitOnAddress() wake races the end of a short wait.
 *
 * A waker thread loops RtlWakeAddressSingle()/RtlWakeAddressAll() on one address while the main thread does short
 * waits on it (timeout 0, then 100 ns), so that wakes race with the waits timing out. After each wait the thread
 * checks whether the wake left an alert pending on it:
 *  - "probe" pass: NtWaitForAlertByThreadId(NULL, 0) returns STATUS_ALERTED if an alert is pending (and takes it);
 *  - "early" pass: a 100 ns RtlWaitOnAddress() on an unrelated address must time out. Anything else is an early
 *    return: in the game, that is what ends the unlooped SleepConditionVariableCS(INFINITE) early.
 * Same measurement as the proposed Wine test test_wait_on_address_raced_wake(), built to run on Windows and Wine.
 *
 * Usage: raced_wake_probe.exe [iterations per pass, default 20000] [seconds cap per pass, default 3]
 */
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000)
#endif
#ifndef STATUS_ALERTED
#define STATUS_ALERTED ((NTSTATUS)0x00000101)
#endif
#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102)
#endif

static NTSTATUS (NTAPI *pRtlWaitOnAddress)( const void *, const void *, SIZE_T, const LARGE_INTEGER * );
static void (NTAPI *pRtlWakeAddressSingle)( const void * );
static void (NTAPI *pRtlWakeAddressAll)( const void * );
static NTSTATUS (NTAPI *pNtWaitForAlertByThreadId)( const void *, const LARGE_INTEGER * );
static NTSTATUS (NTAPI *pRtlGetVersion)( RTL_OSVERSIONINFOW * );
static const char * (CDECL *pwine_get_version)(void);

static volatile LONG address, stop;
static BOOL wake_all;

struct result
{
    unsigned int waits, success, timeout, other, stale, early;
};

static DWORD WINAPI waker_thread( void *arg )
{
    while (!stop)
    {
        if (wake_all) pRtlWakeAddressAll( (const void *)&address );
        else pRtlWakeAddressSingle( (const void *)&address );
    }
    return 0;
}

/* Race short waits against the waker. With probe, look for a pending alert after each wait; otherwise do the
 * 100 ns wait on another address, which must time out. */
static void run_pass( LONGLONG wait_timeout, BOOL probe, unsigned int iterations, DWORD cap_ms, struct result *r )
{
    static const LARGE_INTEGER zero;
    LARGE_INTEGER timeout;
    LONG compare = 0, other = 0;
    DWORD start = GetTickCount();
    NTSTATUS status;
    unsigned int i;

    memset( r, 0, sizeof(*r) );
    for (i = 0; i < iterations && GetTickCount() - start < cap_ms; i++)
    {
        timeout.QuadPart = wait_timeout;
        status = pRtlWaitOnAddress( (const void *)&address, &compare, sizeof(compare), &timeout );
        r->waits++;
        if (status == STATUS_SUCCESS) r->success++;
        else if (status == STATUS_TIMEOUT) r->timeout++;
        else r->other++;

        if (probe)
        {
            if (pNtWaitForAlertByThreadId( NULL, &zero ) == STATUS_ALERTED) r->stale++;
        }
        else
        {
            timeout.QuadPart = -1;
            if (pRtlWaitOnAddress( &other, &compare, sizeof(compare), &timeout ) != STATUS_TIMEOUT) r->early++;
        }
    }
}

int main( int argc, char **argv )
{
    unsigned int iterations = argc > 1 ? atoi( argv[1] ) : 20000, total_stale = 0, total_early = 0, i;
    DWORD cap_ms = (argc > 2 ? atoi( argv[2] ) : 3) * 1000;
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    RTL_OSVERSIONINFOW ver = { sizeof(ver) };
    struct result r;
    HANDLE thread;

    setvbuf( stdout, NULL, _IONBF, 0 ); /* show each pass as it completes, also through a pipe */

    pRtlWaitOnAddress = (void *)GetProcAddress( ntdll, "RtlWaitOnAddress" );
    pRtlWakeAddressSingle = (void *)GetProcAddress( ntdll, "RtlWakeAddressSingle" );
    pRtlWakeAddressAll = (void *)GetProcAddress( ntdll, "RtlWakeAddressAll" );
    pNtWaitForAlertByThreadId = (void *)GetProcAddress( ntdll, "NtWaitForAlertByThreadId" );
    pRtlGetVersion = (void *)GetProcAddress( ntdll, "RtlGetVersion" );
    pwine_get_version = (void *)GetProcAddress( ntdll, "wine_get_version" );
    if (!pRtlWaitOnAddress || !pRtlWakeAddressSingle || !pRtlWakeAddressAll || !pNtWaitForAlertByThreadId)
    {
        fprintf( stderr, "ntdll lacks RtlWaitOnAddress or NtWaitForAlertByThreadId (needs Windows 8 or later)\n" );
        return 1;
    }

    if (pRtlGetVersion) pRtlGetVersion( &ver );
    printf( "OS %lu.%lu build %lu%s%s, %lu processors\n", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber,
            pwine_get_version ? ", Wine " : "", pwine_get_version ? pwine_get_version() : "",
            (unsigned long)GetActiveProcessorCount( ALL_PROCESSOR_GROUPS ) );

    /* A pending alert from anything before us would be misread as ours. */
    {
        static const LARGE_INTEGER zero;
        pNtWaitForAlertByThreadId( NULL, &zero );
    }

    for (i = 0; i < 8; i++)
    {
        LONGLONG wait_timeout = (i & 4) ? -1 : 0;
        BOOL probe = !(i & 1);

        wake_all = !!(i & 2);
        printf( "timeout %3d ns, wake %-6s, %s: ", (int)-wait_timeout * 100, wake_all ? "all" : "single",
                probe ? "probe" : "early" );
        stop = 0;
        thread = CreateThread( NULL, 0, waker_thread, NULL, 0, NULL );
        run_pass( wait_timeout, probe, iterations, cap_ms, &r );
        InterlockedExchange( &stop, 1 );
        WaitForSingleObject( thread, INFINITE );
        CloseHandle( thread );

        printf( "%6u waits (%6u woken, %6u timed out, %u other)", r.waits, r.success, r.timeout, r.other );
        if (probe) printf( ", %u stale alerts\n", r.stale );
        else printf( ", %u early returns on another address\n", r.early );
        total_stale += r.stale;
        total_early += r.early;
    }

    printf( "total: %u stale alerts, %u early returns\n", total_stale, total_early );
    return 0;
}
