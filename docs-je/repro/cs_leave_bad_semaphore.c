/* What LeaveCriticalSection() does when it has a waiter to wake and LockSemaphore is not a valid handle.
 *
 * Wine's RtlpUnWaitCriticalSection() raises a non-continuable STATUS_INVALID_HANDLE there. That is how Helldivers 2
 * dies when its D3D12 task dispatcher leaves a critical section in a stack frame the renderer thread has already
 * reused. Wine's kernel32:sync test checks that Windows raises for a bad LockSemaphore when EnterCriticalSection()
 * has to wait; nothing checks the leave. This probe does, three ways, each in a process of its own:
 *
 *   waiter      a second thread blocks in EnterCriticalSection(); the owner then sets LockSemaphore to 0xdeadbeef
 *               and leaves.
 *   stale       the leave runs on the bytes a reused stack frame holds under Wine: pointers between 4 and 8 GB,
 *               where Wine puts the game's image and the renderer's stack, over LockCount/RecursionCount and
 *               over LockSemaphore.
 *   stale-high  the same with pointers where Windows puts them, above 8 GB.
 *
 * Usage: cs_leave_bad_semaphore.exe [waiter|stale|stale-high]   (no argument: all three, each in a child process)
 * Prints one line per mode: the fields before the leave, whether it returned or raised, and the fields after. */
#include <windows.h>
#include <winternl.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf jb;
static volatile DWORD raised_code, raised_flags;

static LONG WINAPI handler( EXCEPTION_POINTERS *ep )
{
    raised_code = ep->ExceptionRecord->ExceptionCode;
    raised_flags = ep->ExceptionRecord->ExceptionFlags;
    longjmp( jb, 1 );
    return EXCEPTION_CONTINUE_SEARCH;
}

static void fields( const char *label, const CRITICAL_SECTION *cs )
{
    printf( " %s: LockCount %ld (%#lx) RecursionCount %ld OwningThread %p LockSemaphore %p,", label,
            cs->LockCount, (unsigned long)cs->LockCount, cs->RecursionCount, cs->OwningThread, cs->LockSemaphore );
}

static CRITICAL_SECTION waiter_cs;

static DWORD WINAPI waiter_thread( void *arg )
{
    EnterCriticalSection( &waiter_cs );
    LeaveCriticalSection( &waiter_cs );
    return 0;
}

/* Leaves cs under a vectored handler; returns 1 if the leave raised. */
static int leave( CRITICAL_SECTION *cs )
{
    void *vh = AddVectoredExceptionHandler( TRUE, handler );
    int ret = 0;

    if (!setjmp( jb )) LeaveCriticalSection( cs );
    else ret = 1;
    RemoveVectoredExceptionHandler( vh );
    return ret;
}

static int run_mode( const char *mode )
{
    CRITICAL_SECTION stale_cs, *cs = &stale_cs;
    HANDLE thread = NULL;
    int ret;

    if (!strcmp( mode, "waiter" ))
    {
        cs = &waiter_cs;
        InitializeCriticalSectionAndSpinCount( cs, 0 );
        EnterCriticalSection( cs );
        thread = CreateThread( NULL, 0, waiter_thread, NULL, 0, NULL );
        Sleep( 500 );   /* let it block */
        fields( "before, with a waiter", cs );
        cs->LockSemaphore = (HANDLE)0xdeadbeef;
    }
    else if (!strcmp( mode, "stale" ) || !strcmp( mode, "stale-high" ))
    {
        BOOL high = !strcmp( mode, "stale-high" );

        memset( cs, 0, sizeof(*cs) );
        cs->DebugInfo = (RTL_CRITICAL_SECTION_DEBUG *)-1;   /* no debug info, as since Windows 8 */
        /* LockCount is the low half of the pointer, RecursionCount its high half. */
        ULONGLONG pointer = high ? 0x000000a83c5ff8b8ull : 0x000000013ff998b8ull;
        memcpy( &cs->LockCount, &pointer, sizeof(pointer) );
        cs->LockSemaphore = (HANDLE)(ULONG_PTR)(high ? 0x00007ff6c1d20010ull : 0x0000000141735448ull);
        fields( "before", cs );
    }
    else
    {
        printf( "unknown mode %s\n", mode );
        return 2;
    }

    ret = leave( cs );
    if (ret) printf( " LeaveCriticalSection() raised %#lx, flags %#lx,", raised_code, raised_flags );
    else printf( " LeaveCriticalSection() returned," );
    fields( "after", cs );
    printf( "\n" );
    fflush( stdout );
    if (thread) TerminateThread( thread, 0 );
    return ret;
}

int main( int argc, char **argv )
{
    static const char *modes[] = { "waiter", "stale", "stale-high" };
    NTSTATUS (NTAPI *pRtlGetVersion)( RTL_OSVERSIONINFOW * );
    const char * (CDECL *pwine_get_version)(void);
    HMODULE ntdll = GetModuleHandleA( "ntdll.dll" );
    RTL_OSVERSIONINFOW v = { sizeof(v) };
    char exe[MAX_PATH], cmd[MAX_PATH + 32];
    unsigned int i;

    if (argc > 1)
    {
        printf( "%s:", argv[1] );
        fflush( stdout );
        return run_mode( argv[1] );
    }

    pRtlGetVersion = (void *)GetProcAddress( ntdll, "RtlGetVersion" );
    pwine_get_version = (void *)GetProcAddress( ntdll, "wine_get_version" );
    pRtlGetVersion( &v );
    printf( "Windows %lu.%lu.%lu%s%s\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber,
            pwine_get_version ? ", Wine " : "", pwine_get_version ? pwine_get_version() : "" );
    fflush( stdout );

    GetModuleFileNameA( NULL, exe, sizeof(exe) );
    for (i = 0; i < ARRAYSIZE(modes); i++)
    {
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        DWORD code = 0;

        snprintf( cmd, sizeof(cmd), "\"%s\" %s", exe, modes[i] );
        if (!CreateProcessA( exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
        {
            printf( "%s: CreateProcess failed, error %lu\n", modes[i], GetLastError() );
            continue;
        }
        if (WaitForSingleObject( pi.hProcess, 10000 ))
        {
            TerminateProcess( pi.hProcess, 3 );
            printf( " timed out after 10 s\n" );
        }
        GetExitCodeProcess( pi.hProcess, &code );
        if (code > 1 && code != 3) printf( " (child exited with %#lx)\n", code );
        fflush( stdout );
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
    }
    return 0;
}
