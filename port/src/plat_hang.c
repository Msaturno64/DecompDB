/*
 * Diagnostics (Windows): BT3_HANG=<seconds>. When the vertical blank stops advancing for that long, the game
 * thread is made to fault where it is: as if the place it is stuck at had called a function that writes to
 * address 0. The crash report (plat_crash.c) then shows the stuck call chain, innermost first. Not game state.
 *
 * BT3_PROFILE=<file>: about every millisecond the game thread is stopped for a moment and its call chain (up to 24
 * return addresses, innermost first) written to <file>, one sample a line after the vertical blank's number.
 * research/tests/profile.py turns that into time by function and by source file (self and with callees).
 */
#ifdef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include "port_host.h"

extern unsigned gPortVBlanks; /* plat_stub.c */

PORT_HOST static HANDLE sGame;
PORT_HOST static int sStarted;

static void hang_fault(void) {
    *(volatile int *)0 = 0;
}

static DWORD WINAPI hang_watch(void *arg) {
    int secs = (int)(intptr_t)arg, still = 0;
    unsigned last = gPortVBlanks;
    CONTEXT c;

    for (;;) {
        Sleep(1000);
        if (gPortVBlanks != last) {
            last = gPortVBlanks;
            still = 0;
            continue;
        }
        if (++still < secs) {
            continue;
        }
        fprintf(stderr, "bt3: hang: no vertical blank for %d s (stuck after blank %u): faulting the game thread to show where\n",
                secs, last);
        SuspendThread(sGame);
        c.ContextFlags = CONTEXT_FULL;
        GetThreadContext(sGame, &c);
        c.Rsp -= 8; /* a call from the stuck place: its address is the return address */
        *(DWORD64 *)(uintptr_t)c.Rsp = c.Rip;
        c.Rip = (DWORD64)(uintptr_t)hang_fault;
        SetThreadContext(sGame, &c);
        ResumeThread(sGame);
        return 0;
    }
}

/* (Windows Vista and later: the headers declare it only for _WIN32_WINNT >= 0x0600) */
HANDLE WINAPI CreateWaitableTimerExW(LPSECURITY_ATTRIBUTES attr, LPCWSTR name, DWORD flags, DWORD access);
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif
#define PROF_DEPTH 24

static DWORD WINAPI prof_sample(void *arg) {
    FILE *fp = (FILE *)arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    DWORD64 pc[PROF_DEPTH], base;
    CONTEXT c;
    int n, k, lines = 0;

    due.QuadPart = -10000; /* 1 ms */
    if (timer != NULL) {
        SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE);
    }
    for (;;) {
        if (timer != NULL) {
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1); /* (no high resolution timer: the system tick, ~16 ms) */
        }
        n = 0;
        SuspendThread(sGame);
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(sGame, &c)) {
            while (n < PROF_DEPTH && c.Rip != 0) {
                PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, NULL);
                void *data;
                DWORD64 frame;

                pc[n++] = c.Rip;
                if (f == NULL) { /* a leaf function: the return address is on top of the stack */
                    c.Rip = *(DWORD64 *)(uintptr_t)c.Rsp;
                    c.Rsp += 8;
                } else {
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, NULL);
                }
            }
        }
        ResumeThread(sGame); /* (written after: the game thread may hold a lock the C library needs) */
        fprintf(fp, "%u", gPortVBlanks);
        for (k = 0; k < n; k++) {
            fprintf(fp, " %llx", (unsigned long long)pc[k]);
        }
        fputc('\n', fp);
        if (++lines % 1000 == 0) {
            fflush(fp);
        }
    }
}

/* Port_Trace, every vertical blank, on the game thread. */
void Port_HangWatch(void) {
    const char *e;

    if (sStarted) {
        return;
    }
    sStarted = 1;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sGame, 0, FALSE, DUPLICATE_SAME_ACCESS);
    e = getenv("BT3_PROFILE");
    if (e != NULL && *e != '\0') {
        FILE *fp = fopen(e, "w");

        if (fp != NULL) {
            CreateThread(NULL, 0, prof_sample, fp, 0, NULL);
        }
    }
    e = getenv("BT3_HANG");
    if (e == NULL || atoi(e) <= 0) {
        return;
    }
    CreateThread(NULL, 0, hang_watch, (void *)(intptr_t)atoi(e), 0, NULL);
}
#else
void Port_HangWatch(void) {
}
#endif
