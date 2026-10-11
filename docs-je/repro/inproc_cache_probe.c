/* Create many events and wait on each one once, so that ntdll's in-process
 * sync cache (dlls/ntdll/unix/sync.c, with /dev/ntsync in use) has to map a
 * second block of entries. The first version of patch 5 mapped that block at
 * the entries' exact size, 65520 bytes, which anon_mmap_alloc() rejects with
 * an assertion; the process aborts at the wait on the handle with index 3276.
 * Expected everywhere: "waited on all 5000 events: OK".
 * Usage: inproc_cache_probe.exe [events, default 5000] */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main( int argc, char **argv )
{
    unsigned int n = argc > 1 ? strtoul( argv[1], NULL, 0 ) : 5000, i;
    UINT_PTR highest = 0;
    HANDLE *h = malloc( n * sizeof(*h) );

    for (i = 0; i < n; i++)
    {
        if (!(h[i] = CreateEventW( NULL, TRUE, TRUE, NULL )))
        {
            printf( "CreateEvent %u failed, error %lu\n", i, GetLastError() );
            return 1;
        }
        if ((UINT_PTR)h[i] > highest) highest = (UINT_PTR)h[i];
    }
    printf( "created %u events, highest handle %#lx\n", n, (unsigned long)highest );
    fflush( stdout );

    for (i = 0; i < n; i++)
    {
        DWORD ret = WaitForSingleObject( h[i], 0 );
        if (ret != WAIT_OBJECT_0)
        {
            printf( "wait on event %u (handle %p) returned %lu\n", i, h[i], ret );
            return 1;
        }
        if (!(i % 1000)) { printf( "  waited on %u (handle %p)\n", i, h[i] ); fflush( stdout ); }
    }
    printf( "waited on all %u events: OK\n", n );
    for (i = 0; i < n; i++) CloseHandle( h[i] );
    return 0;
}
