/* Cost of the critical section wake guard: uncontended EnterCriticalSection()/LeaveCriticalSection() pairs, and a
 * WaitOnAddress()/WakeByAddressSingle() ping-pong in which the waker holds a heap critical section (tracked, but
 * not on the waiter's stack, so the guard must not engage). */
#include <windows.h>
#include <stdio.h>

static CRITICAL_SECTION heap_cs;
static volatile LONG turn;
static const int rounds = 100000;

static DWORD WINAPI pong( void *arg )
{
    int i;
    for (i = 0; i < rounds; i++)
    {
        LONG mine = 1;
        while (turn != mine) WaitOnAddress( (void *)&turn, (void *)&(LONG){ 0 }, sizeof(LONG), INFINITE );
        EnterCriticalSection( &heap_cs );
        InterlockedExchange( &turn, 0 );
        WakeByAddressSingle( (void *)&turn );
        LeaveCriticalSection( &heap_cs );
    }
    return 0;
}

int main( void )
{
    LARGE_INTEGER freq, t0, t1;
    CRITICAL_SECTION cs;
    HANDLE thread;
    int i;

    QueryPerformanceFrequency( &freq );
    InitializeCriticalSection( &cs );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < 20000000; i++)
    {
        EnterCriticalSection( &cs );
        LeaveCriticalSection( &cs );
    }
    QueryPerformanceCounter( &t1 );
    printf( "enter/leave: %.2f ns per pair\n", (t1.QuadPart - t0.QuadPart) * 1e9 / freq.QuadPart / 20000000 );

    InitializeCriticalSection( &heap_cs );
    thread = CreateThread( NULL, 0, pong, NULL, 0, NULL );
    QueryPerformanceCounter( &t0 );
    for (i = 0; i < rounds; i++)
    {
        EnterCriticalSection( &heap_cs );
        InterlockedExchange( &turn, 1 );
        WakeByAddressSingle( (void *)&turn );
        LeaveCriticalSection( &heap_cs );
        while (turn != 0) WaitOnAddress( (void *)&turn, (void *)&(LONG){ 1 }, sizeof(LONG), INFINITE );
    }
    QueryPerformanceCounter( &t1 );
    WaitForSingleObject( thread, INFINITE );
    printf( "ping-pong: %.2f us per round trip\n", (t1.QuadPart - t0.QuadPart) * 1e6 / freq.QuadPart / rounds );
    return 0;
}
