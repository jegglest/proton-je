/* What threads leave behind in the address space when they end: killed with TerminateThread(), or returning.
 *
 * Usage: killed_thread_stacks.exe [kill|suspended|exit|all] [threads]
 *   kill:      each thread starts, blocks in Sleep() and is killed with TerminateThread().
 *   suspended: Helldivers 2's pattern: NtCreateThreadEx() with no start address, suspended, every other one also hidden
 *              from debuggers, then NtTerminateThread() right away; the threads never run.
 *   exit:      each thread returns normally, for comparison.
 *   all (default): the three, in that order. threads: default 500.
 *
 * Counts the private allocations and committed bytes in the address space (VirtualQuery() over all of it) and the
 * working set before and after each batch. Windows frees a killed thread's stack; Wine before Proton-JE's patch 7 kept
 * two allocations per killed thread, the stack and the kernel stack, about 2 MiB of address space each time. With the
 * patch Wine frees them when the next thread starts or exits, so one more thread is run before the second count.
 */
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef LONG (NTAPI *nt_create_thread_ex)(HANDLE *, ACCESS_MASK, void *, HANDLE, void *, void *, ULONG, SIZE_T, SIZE_T,
                                          SIZE_T, void *);
typedef LONG (NTAPI *nt_terminate_thread)(HANDLE, LONG);

#define THREAD_CREATE_FLAGS_CREATE_SUSPENDED   0x1
#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER 0x4

struct snap { long allocs; SIZE_T committed, working_set; };

static void snapshot(struct snap *s)
{
    MEMORY_BASIC_INFORMATION mbi;
    PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
    char *p = NULL;
    void *last = NULL;

    memset(s, 0, sizeof(*s));
    while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE)
        {
            if (mbi.AllocationBase != last) s->allocs++;
            last = mbi.AllocationBase;
            if (mbi.State == MEM_COMMIT) s->committed += mbi.RegionSize;
        }
        p = (char *)mbi.BaseAddress + mbi.RegionSize;
        if (p < (char *)mbi.BaseAddress) break;
    }
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) s->working_set = pmc.WorkingSetSize;
}

static volatile LONG started;

static DWORD WINAPI sleeper(void *arg)
{
    InterlockedIncrement(&started);
    Sleep(INFINITE);
    return 0;
}

static DWORD WINAPI returner(void *arg)
{
    InterlockedIncrement(&started);
    return 0;
}

static void run_one(LPTHREAD_START_ROUTINE proc)
{
    HANDLE h = CreateThread(NULL, 0, proc, NULL, 0, NULL);
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
}

static void batch(const char *mode, int n)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    nt_create_thread_ex create = (nt_create_thread_ex)GetProcAddress(ntdll, "NtCreateThreadEx");
    nt_terminate_thread terminate = (nt_terminate_thread)GetProcAddress(ntdll, "NtTerminateThread");
    struct snap a, b;
    int i, failed = 0;

    run_one(returner);
    Sleep(100);
    snapshot(&a);
    for (i = 0; i < n; i++)
    {
        HANDLE h = NULL;

        if (!strcmp(mode, "kill"))
        {
            LONG want = started + 1;
            h = CreateThread(NULL, 0, sleeper, NULL, 0, NULL);
            while (started < want) Sleep(0);
            Sleep(2); /* let it block in Sleep() */
            if (!TerminateThread(h, 1)) failed++;
        }
        else if (!strcmp(mode, "suspended"))
        {
            ULONG flags = THREAD_CREATE_FLAGS_CREATE_SUSPENDED | ((i & 1) ? THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER : 0);
            if (create(&h, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(), NULL, NULL, flags, 0, 0, 0, NULL))
            {
                failed++;
                continue;
            }
            terminate(h, 0);
        }
        else h = CreateThread(NULL, 0, returner, NULL, 0, NULL);
        WaitForSingleObject(h, INFINITE);
        CloseHandle(h);
    }
    run_one(returner); /* Wine with patch 7 frees killed threads when the next thread starts or exits */
    Sleep(100);
    snapshot(&b);
    printf("%-9s %d threads%s: private allocations %+ld (%+.2f per thread), committed %+.1f MiB (%+.1f KiB per thread), "
           "working set %+.1f MiB\n", mode, n, failed ? " (some failed)" : "", b.allocs - a.allocs,
           (double)(b.allocs - a.allocs) / n, ((double)b.committed - a.committed) / 1048576,
           ((double)b.committed - a.committed) / 1024 / n, ((double)b.working_set - a.working_set) / 1048576);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";
    int n = argc > 2 ? atoi(argv[2]) : 500;

    if (!strcmp(mode, "all"))
    {
        batch("kill", n);
        batch("suspended", n);
        batch("exit", n);
    }
    else if (!strcmp(mode, "kill") || !strcmp(mode, "suspended") || !strcmp(mode, "exit")) batch(mode, n);
    else
    {
        printf("usage: killed_thread_stacks.exe [kill|suspended|exit|all] [threads]\n");
        return 1;
    }
    return 0;
}
