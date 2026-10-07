/* Timed version of the end of kernel32:sync test_mutex(), plus one corner case. Prints how long each step takes.
 *
 * Usage: mutex_abandon.exe [close|keep|waitall] [iterations]
 *   close (default): a thread owns a mutex and blocks; a second thread waits on the mutex; the mutex handle is
 *                    closed; the owner is killed with TerminateThread(). The waiter must get WAIT_ABANDONED.
 *   keep:            the same with the mutex handle kept open.
 *   waitall:         the mutex is unowned when its handle is closed, and two threads are blocked in wait-all calls
 *                    on it, each with its own unset event. Setting event A lets thread A take the mutex; A is then
 *                    killed; setting event B must give thread B the abandoned mutex.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE mutex, start_event, stop_event, event_a, event_b;
static LARGE_INTEGER freq, t0;

static double ms(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (t.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
}

static const char *abandoned(DWORD ret)
{
    return (ret >= WAIT_ABANDONED_0 && ret < WAIT_ABANDONED_0 + 2) ? " (abandoned)" : "";
}

static DWORD WINAPI owner_proc(void *arg)
{
    DWORD ret = WaitForSingleObject(mutex, INFINITE);
    printf("%9.1f ms  owner:  mutex wait returned %#lx\n", ms(), ret);
    SetEvent(start_event);
    ret = WaitForSingleObject(stop_event, INFINITE);
    printf("%9.1f ms  owner:  stop_event wait returned %#lx (must not happen)\n", ms(), ret);
    return 0;
}

static DWORD WINAPI waiter_proc(void *arg)
{
    DWORD ret = WaitForSingleObject(mutex, INFINITE);
    printf("%9.1f ms  waiter: mutex wait returned %#lx%s\n", ms(), ret, abandoned(ret));
    SetEvent(start_event);
    ret = WaitForSingleObject(stop_event, INFINITE);
    printf("%9.1f ms  waiter: stop_event wait returned %#lx\n", ms(), ret);
    return 0;
}

static DWORD WINAPI waitall_a_proc(void *arg)
{
    HANDLE objs[2] = { mutex, event_a };
    DWORD ret = WaitForMultipleObjects(2, objs, TRUE, INFINITE);
    printf("%9.1f ms  A:      wait-all on mutex + event A returned %#lx\n", ms(), ret);
    SetEvent(start_event);
    ret = WaitForSingleObject(stop_event, INFINITE);
    printf("%9.1f ms  A:      stop_event wait returned %#lx (must not happen)\n", ms(), ret);
    return 0;
}

static DWORD WINAPI waitall_b_proc(void *arg)
{
    HANDLE objs[2] = { mutex, event_b };
    DWORD ret = WaitForMultipleObjects(2, objs, TRUE, INFINITE);
    printf("%9.1f ms  B:      wait-all on mutex + event B returned %#lx%s\n", ms(), ret, abandoned(ret));
    SetEvent(start_event);
    ret = WaitForSingleObject(stop_event, INFINITE);
    printf("%9.1f ms  B:      stop_event wait returned %#lx\n", ms(), ret);
    return 0;
}

static int step(const char *what, DWORD ret, DWORD expect)
{
    printf("%9.1f ms  main:   %s %#lx (expect %#lx)%s\n", ms(), what, ret, expect, ret == expect ? "" : "  <-- FAILED");
    return ret != expect;
}

static void create_events(void)
{
    QueryPerformanceCounter(&t0);
    start_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    stop_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    event_a = CreateEventW(NULL, FALSE, FALSE, NULL);
    event_b = CreateEventW(NULL, FALSE, FALSE, NULL);
    mutex = CreateMutexA(NULL, FALSE, NULL);
}

static void close_events(void)
{
    CloseHandle(start_event);
    CloseHandle(stop_event);
    CloseHandle(event_a);
    CloseHandle(event_b);
}

static int run(int close_mutex)
{
    HANDLE owner, waiter;
    int failures = 0;

    create_events();
    owner = CreateThread(NULL, 0, owner_proc, NULL, 0, NULL);
    failures += step("owner started, start_event wait", WaitForSingleObject(start_event, 1000), 0);

    waiter = CreateThread(NULL, 0, waiter_proc, NULL, 0, NULL);
    failures += step("waiter blocked, start_event wait", WaitForSingleObject(start_event, 100), WAIT_TIMEOUT);

    if (close_mutex)
    {
        CloseHandle(mutex);
        failures += step("mutex handle closed, start_event wait", WaitForSingleObject(start_event, 100), WAIT_TIMEOUT);
    }

    printf("%9.1f ms  main:   TerminateThread(owner) -> %d\n", ms(), TerminateThread(owner, 0));
    failures += step("owner thread handle wait", WaitForSingleObject(owner, 5000), 0);
    failures += step("start_event wait (waiter got the mutex)", WaitForSingleObject(start_event, 5000), 0);

    SetEvent(stop_event);
    failures += step("waiter thread handle wait", WaitForSingleObject(waiter, 5000), 0);

    CloseHandle(owner);
    CloseHandle(waiter);
    if (!close_mutex) CloseHandle(mutex);
    close_events();
    return failures;
}

static int run_waitall(void)
{
    HANDLE a, b;
    int failures = 0;

    create_events();
    a = CreateThread(NULL, 0, waitall_a_proc, NULL, 0, NULL);
    b = CreateThread(NULL, 0, waitall_b_proc, NULL, 0, NULL);
    failures += step("A and B blocked, start_event wait", WaitForSingleObject(start_event, 100), WAIT_TIMEOUT);

    CloseHandle(mutex);   /* nobody owns the mutex here */
    failures += step("mutex handle closed, start_event wait", WaitForSingleObject(start_event, 100), WAIT_TIMEOUT);

    SetEvent(event_a);
    failures += step("event A set, start_event wait (A owns the mutex)", WaitForSingleObject(start_event, 1000), 0);

    printf("%9.1f ms  main:   TerminateThread(A) -> %d\n", ms(), TerminateThread(a, 0));
    failures += step("A thread handle wait", WaitForSingleObject(a, 5000), 0);

    SetEvent(event_b);
    failures += step("event B set, start_event wait (B got the abandoned mutex)", WaitForSingleObject(start_event, 5000), 0);

    SetEvent(stop_event);
    failures += step("B thread handle wait", WaitForSingleObject(b, 5000), 0);

    CloseHandle(a);
    CloseHandle(b);
    close_events();
    return failures;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "close";
    int iterations = argc > 2 ? atoi(argv[2]) : 1, i, failures = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&freq);
    if (strcmp(mode, "close") && strcmp(mode, "keep") && strcmp(mode, "waitall"))
    {
        printf("usage: mutex_abandon.exe [close|keep|waitall] [iterations]\n");
        return 2;
    }
    printf("mode: %s, %d iteration(s)\n", mode, iterations);
    for (i = 0; i < iterations; i++)
    {
        printf("--- iteration %d\n", i + 1);
        if (!strcmp(mode, "waitall")) failures += run_waitall();
        else failures += run(!strcmp(mode, "close"));
    }
    printf("%d failed step(s)\n", failures);
    return failures;
}
