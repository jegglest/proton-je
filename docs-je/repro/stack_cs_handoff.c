/* Helldivers 2's renderer/dispatcher handoff, with the race window forced open.
 *
 * The "renderer" (main thread) puts an object holding a critical section and a flag on its stack, hands it to the
 * "dispatcher" thread and waits for the flag with WaitOnAddress(). The dispatcher enters the critical section, sets
 * the flag, wakes the renderer, stalls for delay_us and only then leaves the critical section. A renderer that
 * returns from WaitOnAddress() before that leave has finished would, in the game, reuse the frame under it.
 *
 * Usage: stack_cs_handoff.exe [iterations] [delay_us] [mode]
 *   mode "stack" (default): the critical section is on the renderer's stack, as in the game.
 *   mode "heap": the same handoff with the object on the heap; the guard must not engage.
 *   mode "condvar": SleepConditionVariableCS() on a critical section on the renderer's stack, with a third thread
 *                   contending for it; the guard must not stall the renderer.
 * Prints how many handoffs returned early and the slowest wake, and exits 1 if any returned early in stack mode. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct handoff
{
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    volatile LONG flag;
};

static struct handoff * volatile current;
static volatile LONG leave_done, stop;
static int delay_us = 200;
static const char *mode = "stack";
static LARGE_INTEGER freq;

static LONGLONG now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter( &t );
    return t.QuadPart * 1000000 / freq.QuadPart;
}

static void stall( int us )
{
    LONGLONG end = now_us() + us;
    while (now_us() < end) YieldProcessor();
}

static DWORD WINAPI dispatcher( void *arg )
{
    while (!stop)
    {
        struct handoff *h;

        if (!(h = current)) { YieldProcessor(); continue; }
        current = NULL;

        /* Give the renderer time to block, so that it is the wake that releases it. */
        stall( 500 );
        EnterCriticalSection( &h->cs );
        InterlockedExchange( &h->flag, 1 );
        if (!strcmp( mode, "condvar" )) WakeConditionVariable( &h->cv );
        else WakeByAddressSingle( (void *)&h->flag );
        stall( delay_us );
        LeaveCriticalSection( &h->cs );
        InterlockedExchange( &leave_done, 1 );
    }
    return 0;
}

static struct handoff * volatile contended;
static volatile LONG contending;

/* condvar mode: keeps entering and leaving the renderer's critical section. */
static DWORD WINAPI contender( void *arg )
{
    while (!stop)
    {
        struct handoff *h;

        InterlockedExchange( &contending, 1 );
        if (!(h = contended))
        {
            InterlockedExchange( &contending, 0 );
            YieldProcessor();
            continue;
        }
        EnterCriticalSection( &h->cs );
        stall( 20 );
        LeaveCriticalSection( &h->cs );
        InterlockedExchange( &contending, 0 );
        stall( 50 );  /* don't starve the others: critical sections aren't fair */
    }
    return 0;
}

int main( int argc, char **argv )
{
    int iterations = argc > 1 ? atoi( argv[1] ) : 200, early = 0, i;
    LONGLONG slowest = 0;
    HANDLE threads[2];

    if (argc > 2) delay_us = atoi( argv[2] );
    if (argc > 3) mode = argv[3];
    QueryPerformanceFrequency( &freq );
    threads[0] = CreateThread( NULL, 0, dispatcher, NULL, 0, NULL );
    threads[1] = CreateThread( NULL, 0, contender, NULL, 0, NULL );

    for (i = 0; i < iterations; i++)
    {
        struct handoff on_stack, *h = &on_stack;
        static const LONG zero;
        LONGLONG start, took;
        LONG done;

        if (!strcmp( mode, "heap" )) h = malloc( sizeof(*h) );
        InitializeCriticalSection( &h->cs );
        InitializeConditionVariable( &h->cv );
        h->flag = 0;
        leave_done = 0;

        start = now_us();
        if (!strcmp( mode, "condvar" ))
        {
            EnterCriticalSection( &h->cs );
            contended = h;
            current = h;
            while (!h->flag) SleepConditionVariableCS( &h->cv, &h->cs, INFINITE );
            done = leave_done;
            LeaveCriticalSection( &h->cs );
            InterlockedExchangePointer( (void **)&contended, NULL );
            while (contending) YieldProcessor();
        }
        else
        {
            current = h;
            while (!h->flag) WaitOnAddress( (void *)&h->flag, (void *)&zero, sizeof(LONG), INFINITE );
            done = leave_done;
        }
        took = now_us() - start;
        if (took > slowest) slowest = took;
        if (!done) early++;

        /* Keep the object alive until the dispatcher is out of LeaveCriticalSection(). */
        while (!leave_done) YieldProcessor();
        Sleep( 0 );
        DeleteCriticalSection( &h->cs );
        if (h != &on_stack) free( h );
    }

    stop = 1;
    WaitForMultipleObjects( 2, threads, TRUE, INFINITE );
    printf( "mode %s, delay %d us: %d of %d handoffs returned before the leave, slowest wake %lld us\n",
            mode, delay_us, early, iterations, slowest );
    return !strcmp( mode, "stack" ) && early ? 1 : 0;
}
