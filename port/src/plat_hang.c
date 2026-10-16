/*
 * Diagnostics (Windows): BT3_HANG=<seconds>. When the vertical blank stops advancing for that long, the game
 * thread is made to fault where it is: as if the place it is stuck at had called a function that writes to
 * address 0. The crash report (plat_crash.c) then shows the stuck call chain, innermost first. Not game state.
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

/* Port_Trace, every vertical blank, on the game thread. */
void Port_HangWatch(void) {
    const char *e;

    if (sStarted) {
        return;
    }
    sStarted = 1;
    e = getenv("BT3_HANG");
    if (e == NULL || atoi(e) <= 0) {
        return;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sGame, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, hang_watch, (void *)(intptr_t)atoi(e), 0, NULL);
}
#else
void Port_HangWatch(void) {
}
#endif
