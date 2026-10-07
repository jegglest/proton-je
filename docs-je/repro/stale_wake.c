/* Reproducer for spurious SleepConditionVariableCS() wakeups caused by stale thread alerts.
 *
 * Each worker alternates between:
 *  1. a short timed wait on a shared "noise" condition variable that a pinger thread keeps waking, so that
 *     timeouts race with RtlWakeAddressAll() dequeuing the waiter; and
 *  2. the Space Marine 2 engine's future pattern: `if (!done) SleepConditionVariableCS(INFINITE)`, without a
 *     predicate loop, completed by a dedicated completer thread.
 * A return from (2) while the future is still pending is a spurious wakeup; in the game that is where the
 * waiting function returns and the loader's callback later writes into its dead stack frame.
 *
 * Noise modes: "timed" = (1) above; "poll" = tcmalloc-style spinlock that polls with WaitOnAddress(..., 0) and
 * releases with WakeByAddressSingle(), taken by allocator threads and by each worker right before its future wait
 * (as in the game); "both" (default).
 *
 * Usage: stale_wake.exe [seconds] [workers] [timed|poll|both]
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct future
{
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
    volatile LONG state; /* 0 pending, 2 done */
};

struct worker
{
    struct future f;
    HANDLE go;           /* auto-reset event: worker -> completer */
    LONG64 futures, spurious;
    DWORD seed;
};

static CRITICAL_SECTION noise_cs;
static CONDITION_VARIABLE noise_cv;
static volatile LONG stop, stop_completers;
static volatile LONG alloc_lock;
static BOOL use_timed = TRUE, use_poll = TRUE;

/* tcmalloc-style spinlock: poll with a zero timeout while contended. */
static void alloc_lock_acquire(void)
{
    LONG locked = 1;
    while (InterlockedCompareExchange( &alloc_lock, 1, 0 ))
        WaitOnAddress( &alloc_lock, &locked, sizeof(locked), 0 );
}

static void alloc_lock_release(void)
{
    InterlockedExchange( &alloc_lock, 0 );
    WakeByAddressSingle( (void *)&alloc_lock );
}

static void fake_alloc(void)
{
    volatile int i;
    alloc_lock_acquire();
    for (i = 0; i < 50; i++) ;
    alloc_lock_release();
}

static DWORD WINAPI allocator( void *arg )
{
    while (!stop) fake_alloc();
    return 0;
}

static DWORD next_rand( DWORD *seed )
{
    *seed = *seed * 1103515245 + 12345;
    return *seed >> 8;
}

static void spin_us( LARGE_INTEGER freq, DWORD us )
{
    LARGE_INTEGER start, now;
    QueryPerformanceCounter( &start );
    do QueryPerformanceCounter( &now );
    while ((now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart < us);
}

static DWORD WINAPI pinger( void *arg )
{
    LARGE_INTEGER freq;
    DWORD seed = 1;

    QueryPerformanceFrequency( &freq );
    while (!stop)
    {
        WakeAllConditionVariable( &noise_cv );
        spin_us( freq, 200 + next_rand( &seed ) % 1600 );
    }
    return 0;
}

static DWORD WINAPI completer( void *arg )
{
    struct worker *w = arg;
    LARGE_INTEGER freq;
    DWORD seed = w->seed ^ 0x5bd1e995;

    QueryPerformanceFrequency( &freq );
    for (;;)
    {
        WaitForSingleObject( w->go, INFINITE );
        if (stop_completers) break;
        spin_us( freq, 20 + next_rand( &seed ) % 400 );
        EnterCriticalSection( &w->f.cs );
        w->f.state = 2;
        WakeAllConditionVariable( &w->f.cv );
        LeaveCriticalSection( &w->f.cs );
    }
    return 0;
}

static DWORD WINAPI worker_thread( void *arg )
{
    struct worker *w = arg;

    while (!stop)
    {
        /* 1: timed wait racing with the pinger's wakes. */
        if (use_timed)
        {
            EnterCriticalSection( &noise_cs );
            SleepConditionVariableCS( &noise_cv, &noise_cs, 1 );
            LeaveCriticalSection( &noise_cs );
        }
        /* 1b: allocate right before waiting, like the game growing its record vector. */
        if (use_poll)
        {
            int i;
            for (i = 0; i < 4; i++) fake_alloc();
        }

        /* 2: single-shot future wait, as in the game. */
        w->f.state = 0;
        SetEvent( w->go );
        EnterCriticalSection( &w->f.cs );
        if (w->f.state != 2)
        {
            SleepConditionVariableCS( &w->f.cv, &w->f.cs, INFINITE );
            if (w->f.state != 2) w->spurious++;
        }
        /* resynchronise properly before the next round */
        while (w->f.state != 2) SleepConditionVariableCS( &w->f.cv, &w->f.cs, INFINITE );
        LeaveCriticalSection( &w->f.cs );
        w->futures++;
    }
    return 0;
}

int main( int argc, char **argv )
{
    int seconds = argc > 1 ? atoi( argv[1] ) : 30, count = argc > 2 ? atoi( argv[2] ) : 8, i;
    const char *mode = argc > 3 ? argv[3] : "both";
    struct worker *workers = calloc( count, sizeof(*workers) );
    HANDLE *threads = calloc( 2 * count + 1, sizeof(*threads) ), allocators[4] = {0};

    if (!strcmp( mode, "timed" )) use_poll = FALSE;
    else if (!strcmp( mode, "poll" )) use_timed = FALSE;
    LONG64 futures = 0, spurious = 0;

    InitializeCriticalSection( &noise_cs );
    InitializeConditionVariable( &noise_cv );
    for (i = 0; i < count; i++)
    {
        InitializeCriticalSection( &workers[i].f.cs );
        InitializeConditionVariable( &workers[i].f.cv );
        workers[i].go = CreateEventW( NULL, FALSE, FALSE, NULL );
        workers[i].seed = 0x9e3779b9 * (i + 1);
        threads[2 * i] = CreateThread( NULL, 0, completer, &workers[i], 0, NULL );
        threads[2 * i + 1] = CreateThread( NULL, 0, worker_thread, &workers[i], 0, NULL );
    }
    threads[2 * count] = CreateThread( NULL, 0, pinger, NULL, 0, NULL );
    if (use_poll)
        for (i = 0; i < ARRAYSIZE(allocators); i++)
            allocators[i] = CreateThread( NULL, 0, allocator, NULL, 0, NULL );

    Sleep( seconds * 1000 );
    stop = 1;
    for (i = 0; i < count; i++) WaitForSingleObject( threads[2 * i + 1], INFINITE );
    stop_completers = 1;
    for (i = 0; i < count; i++)
    {
        SetEvent( workers[i].go );
        WaitForSingleObject( threads[2 * i], INFINITE );
        futures += workers[i].futures;
        spurious += workers[i].spurious;
    }
    WaitForSingleObject( threads[2 * count], INFINITE );
    for (i = 0; i < ARRAYSIZE(allocators); i++)
        if (allocators[i]) WaitForSingleObject( allocators[i], INFINITE );

    printf( "%s: %d s, %d workers: %lld future waits, %lld returned while still pending (spurious)\n",
            mode, seconds, count, (long long)futures, (long long)spurious );
    fflush( stdout );
    return 0;
}
