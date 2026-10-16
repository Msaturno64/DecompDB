/*
 * Several fighters on the field at once (branch multi-fighter, SPEC-multi-fighter.md of the project around this
 * repository). This first part only measures, for deciding how much memory and time more fighters need:
 *
 *   BT3_MFSTAT=1   at the end of each battle: the fighters, the game heap's free bytes when the battle was set up
 *                  and the least seen during it, and the time of a battle frame's simulation (Job_Run ..
 *                  BtlGame_Update in Battle_Loop) and of the rest of it (drawing, without the time spent waiting
 *                  for the vertical blank), in microseconds
 *
 * The measuring is not part of the game's state (PORT_HOST): it reads, it never writes what the game reads.
 *
 *   BT3_TEAMS=<a>v<b>  (tests) the attract-demo battle with a members on side 0 and b on side 1, 1..5 each, and
 *                      with every member on the field at once (gPortMultiAll)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "port_host.h"

#if (defined(__x86_64__) || defined(__aarch64__)) && defined(__clang__) /* (the 64-bit builds: game pointers are 4 bytes) */
#undef __ptr32 /* the Windows headers of mingw define these two away */
#undef __uptr
#define GAME_PTR *__ptr32 __uptr
#else
#define GAME_PTR *
#endif

/* Every member of both teams on the field at once instead of one after the other. Game state (not PORT_HOST): a
   restored state must bring back the battle it belongs to. */
int gPortMultiAll;

/* ----------------------------------------------------------------------------------- BT3_HEAPCHECK (diagnostics) */

/* BT3_HEAPCHECK=1: at the end of every battle frame the game heap's blocks are walked. The first block whose header
   or trailer is not whole is reported once, with the block before it (the one that ran over its end) and who asked
   for that one: Heap_Alloc notes its caller (PortMulti_NoteAlloc). Not game state. */
#define HEAP_MAGIC_WORD 0x53484254u /* HEAP_MAGIC, include/sys/heap.h */
extern unsigned gHeapStart[2], gHeapEnd[2]; /* src/sys/heap.c: HeapBlock *, 4 bytes each in the game's memory */
PORT_HOST static int sHeapCheck = -1, sHeapReported;
PORT_HOST static unsigned sAllocData[8192], sAllocCaller[8192];

/* src/sys/heap.c Heap_Alloc: a block's data address and who asked for it (both as numbers: game addresses). */
void PortMulti_NoteAlloc(unsigned data, unsigned caller) {
    unsigned h;

    if (sHeapCheck < 0) {
        sHeapCheck = getenv("BT3_HEAPCHECK") != NULL;
    }
    if (!sHeapCheck || data == 0) {
        return;
    }
    for (h = (data >> 4) % 8192; sAllocData[h] != 0 && sAllocData[h] != data; h = (h + 1) % 8192) {
    }
    sAllocData[h] = data;
    sAllocCaller[h] = caller;
}

static unsigned alloc_caller(unsigned data) {
    unsigned h;

    for (h = (data >> 4) % 8192; sAllocData[h] != 0; h = (h + 1) % 8192) {
        if (sAllocData[h] == data) {
            return sAllocCaller[h];
        }
    }
    return 0;
}

static void heap_check(unsigned long long frame) {
    const unsigned *b;
    unsigned at = gHeapStart[0], prev = 0, size;

    if (sHeapReported || at == 0) {
        return;
    }
    while (at != gHeapEnd[0]) {
        b = (const unsigned *)(uintptr_t)at;
        size = b[4];
        if (b[0] != HEAP_MAGIC_WORD || size < 0x20 || at + size > gHeapEnd[0] ||
            *(const unsigned *)(uintptr_t)(at + size - 4) != size) {
            const unsigned *p = (const unsigned *)(uintptr_t)prev;
            sHeapReported = 1;
            fprintf(stderr, "bt3: heapcheck: block at 0x%08X broken (magic %08X size %X) after battle frame %llu\n", at,
                    b[0], size, frame);
            if (prev != 0) {
                fprintf(stderr, "bt3: heapcheck: the block before: 0x%08X, data 0x%08X, %d bytes asked, used %d, "
                        "allocated from\n0x%016llX (Tenkaichi3Decomp.exe)\n", prev, p[5], (int)p[6], (int)p[1],
                        (unsigned long long)alloc_caller(p[5]));
            }
            return;
        }
        prev = at;
        at += size;
    }
}

/* BT3_HEAPWHO=<address> (with BT3_HEAPCHECK=1): at the end of the battle, the heap block holding that address,
   where in it, and who asked for it; for an address a BT3_SYNCTEST report names. */
static void heap_who(void) {
    const char *e = getenv("BT3_HEAPWHO");
    unsigned want, at = gHeapStart[0], size;
    const unsigned *b;

    if (e == NULL || at == 0) {
        return;
    }
    want = (unsigned)strtoul(e, NULL, 16);
    while (at != gHeapEnd[0]) {
        b = (const unsigned *)(uintptr_t)at;
        size = b[4];
        if (size < 0x20) {
            break;
        }
        if (want >= at && want < at + size) {
            fprintf(stderr, "bt3: heapwho: 0x%08X is byte 0x%X of block data 0x%08X (%d bytes asked, used %d), allocated from\n"
                    "0x%016llX (Tenkaichi3Decomp.exe)\n", want, want - b[5], b[5], (int)b[6], (int)b[1],
                    (unsigned long long)alloc_caller(b[5]));
            return;
        }
        at += size;
    }
    fprintf(stderr, "bt3: heapwho: 0x%08X is not in the game heap\n", want);
}

/* ------------------------------------------------------------------------------------------------- targets */

#define FIGHTER_MAX 10 /* BTL_FIGHTER_MAX, battle/battle.h (a game header: its pointers are not this file's) */

extern int BattleSide_GetCount(void);  /* src/battle/battle_load.c */
extern int BattleSide_GetTeam(int side);
extern int BattleSide_GetObjId(int side);
extern void BtlCharApi_GetPos(int objId, float *out); /* src/battle/btl_capi_a.c */
extern void GAME_PTR BtlChar_Get(int i);              /* src/battle/btl_char_mgr.c */
extern int BtlChar_IsDead(void *chr);

/* Each fighter's opponent beyond the two: the enemy it is set on. Game state (not PORT_HOST): a rollback brings it
   back with the fighters. -1: none yet. */
static int sTarget[FIGHTER_MAX];

/* The opponent of fighter i (BTL_OPP, battle/battle.h). With two fighters the other one, as on the console; with
   more the target, or the first enemy while there is none yet (before the first PortMulti_UpdateTargets). */
int PortMulti_TargetOf(int i) {
    int n = BattleSide_GetCount(), t, k;

    if (n <= 2 || i < 0 || i >= n) {
        return i ^ 1;
    }
    t = sTarget[i];
    if (t < 0 || t >= n || BattleSide_GetTeam(t) == BattleSide_GetTeam(i)) {
        for (k = 0; k < n && BattleSide_GetTeam(k) == BattleSide_GetTeam(i); k++) {
        }
        t = k < n ? k : i ^ 1;
    }
    return t;
}

/* BtlChar_ResetAll: a fight starts (or starts again): nobody is set on anyone. */
void PortMulti_ResetTargets(void) {
    int i;

    for (i = 0; i < FIGHTER_MAX; i++) {
        sTarget[i] = -1;
    }
}

/* Battle_Loop, once a frame before the AI: a fighter keeps its target while that one can fight; otherwise it takes
   the nearest enemy that can (the lowest index of equally near ones, so both players' copies agree). With nobody
   left to fight the target stays as it is. */
void PortMulti_UpdateTargets(void) {
    int n = BattleSide_GetCount(), i, k, best;
    float p[FIGHTER_MAX][4] __attribute__((aligned(16)));
    float d, bestD, dx, dy, dz;

    if (n <= 2) {
        return;
    }
    for (i = 0; i < n; i++) {
        BtlCharApi_GetPos(BattleSide_GetObjId(i), p[i]);
    }
    for (i = 0; i < n; i++) {
        k = sTarget[i];
        if (k >= 0 && k < n && BattleSide_GetTeam(k) != BattleSide_GetTeam(i) && !BtlChar_IsDead(BtlChar_Get(k))) {
            continue;
        }
        best = -1;
        bestD = 0.0f;
        for (k = 0; k < n; k++) {
            if (BattleSide_GetTeam(k) == BattleSide_GetTeam(i) || BtlChar_IsDead(BtlChar_Get(k))) {
                continue;
            }
            dx = p[k][0] - p[i][0];
            dy = p[k][1] - p[i][1];
            dz = p[k][2] - p[i][2];
            d = dx * dx + dy * dy + dz * dz;
            if (best < 0 || d < bestD) {
                best = k;
                bestD = d;
            }
        }
        if (best >= 0) {
            sTarget[i] = best;
        }
    }
}

/* ------------------------------------------------------------------------------------------- BT3_MFLOG (tests) */

/* BT3_MFLOG=<file>: one line per vertical blank of a fight: the blank, then for every fighter its team, health
   and position (x y z as the bits of the floats, hexadecimal) and its target: "<blank> | <team> <hp> <x> <y> <z>
   <target> | ...". A blank
   run again after a rollback is written again; the last line of a blank counts. Not game state (PORT_HOST). */
void PortMulti_Trace(unsigned vblank, int battle) {
    extern int BattleSide_GetCount(void);       /* src/battle/battle_load.c */
    extern int BattleSide_GetTeam(int side);
    extern int BattleSide_GetObjId(int side);
    extern int BtlCharApi_GetHp(int objId);      /* src/battle/btl_capi_a.c */
    extern void BtlCharApi_GetPos(int objId, float *out);
    PORT_HOST static FILE *fp;
    PORT_HOST static int tried;
    float q[4] __attribute__((aligned(16)));
    unsigned u[3];
    int i, obj;

    if (!tried) {
        tried = 1;
        fp = getenv("BT3_MFLOG") != NULL ? fopen(getenv("BT3_MFLOG"), "w") : NULL;
    }
    if (fp == NULL || !battle) {
        return;
    }
    fprintf(fp, "%u", vblank);
    for (i = 0; i < BattleSide_GetCount(); i++) {
        obj = BattleSide_GetObjId(i);
        BtlCharApi_GetPos(obj, q);
        memcpy(u, q, sizeof(u)); /* (the bits: this file has no floating-point unit to print with) */
        fprintf(fp, " | %d %d %08x %08x %08x %d", BattleSide_GetTeam(i), BtlCharApi_GetHp(obj), u[0], u[1], u[2],
                PortMulti_TargetOf(i));
    }
    fputc('\n', fp);
    fflush(fp);
}

/* ------------------------------------------------------------------------------------------- BT3_TEAMS (tests) */

typedef struct { int word[25]; } DemoMember; /* BattleMember, 0x64 bytes (include/battle/battle.h); word 0 = chara */
extern DemoMember GAME_PTR BattleSide_GetMember(int side, int index);
/* (a pointer ARGUMENT of a game function is 8 bytes in the call even with ptr32: the callee reads the whole
   stack slot, so it is declared as an ordinary pointer here; a returned game pointer is 4 bytes, GAME_PTR) */
extern void BattleSetup_SetSide(int side, int control, int pad, int memberCount, int unk1FC, int unk200, int lead,
                                void *bits);
extern void BattleSetup_Finish(void);

/* Characters for the added members: the attract demo's own pairings (src/battle/view_b_c.c), side 0's column and
   side 1's, so a run with the same BT3_TEAMS is the same battle every time. */
static const int kDemoExtra[2][5] = {{0x0A, 0x2A, 0x19, 0x27, 0x17}, {0x93, 0x6C, 0x5C, 0x60, 0x6F}};

/* headless.c, after Demo_SetupBattle: a members on side 0 and b on side 1. Each added member is a copy of the
   side's first (its CPU level, health and items) with another character. */
void PortMulti_DemoTeams(const char *teams) {
    int count[2], side, i;
    char rest;

    if (sscanf(teams, "%dv%d%c", &count[0], &count[1], &rest) != 2 || count[0] < 1 || count[0] > 5 || count[1] < 1 ||
        count[1] > 5) {
        fprintf(stderr, "bt3: BT3_TEAMS=%s: expected <a>v<b> with 1..5 on each side, e.g. 2v1\n", teams);
        exit(2);
    }
    for (side = 0; side < 2; side++) {
        BattleSetup_SetSide(side, 2, side, count[side], 1, 1, 0, NULL); /* as Demo_SetupBattle: CPU, pad = side */
        for (i = 1; i < count[side]; i++) {
            *BattleSide_GetMember(side, i) = *BattleSide_GetMember(side, 0);
            BattleSide_GetMember(side, i)->word[0] = kDemoExtra[side][i - 1];
            BattleSide_GetMember(side, i)->word[1] = 0; /* costume */
            BattleSide_GetMember(side, i)->word[2] = 0; /* variant */
        }
    }
    /* BT3_TEAMS_HP=<percent>[,<percent>...] (tests): the share of its health a member starts with, for quick
       knock-outs. One value: every member; a list: side 0's members, then side 1's, in order (the last repeats). */
    if (getenv("BT3_TEAMS_HP") != NULL) {
        const char *e = getenv("BT3_TEAMS_HP");
        float pct;
        int bits;

        for (side = 0; side < 2; side++) {
            for (i = 0; i < count[side]; i++) {
                pct = (float)atoi(e);
                memcpy(&bits, &pct, sizeof(bits));
                BattleSide_GetMember(side, i)->word[4] = bits; /* BattleMember.health, a float percentage */
                if (strchr(e, ',') != NULL) {
                    e = strchr(e, ',') + 1;
                }
            }
        }
    }
    BattleSetup_Finish();
    gPortMultiAll = 1;
    printf("bt3: teams %dv%d, every member on the field\n", count[0], count[1]);
}

extern int Heap_GetFreeSize(void);  /* src/sys/heap.c: the free blocks of every heap */
extern int BtlChar_GetCount(void);  /* src/battle/btl_char_mgr.c */
extern unsigned long long gPortSleptNs; /* plat_stub.c: time spent waiting for the vertical blank */

PORT_HOST static int sOn = -1;
PORT_HOST static int sHeapStart, sHeapMin;
PORT_HOST static unsigned long long sMark, sSlept, sSimSum, sSimMax, sRestSum, sRestMax, sFrames;

static unsigned long long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

static int on(void) {
    if (sOn < 0) {
        sOn = getenv("BT3_MFSTAT") != NULL;
    }
    return sOn;
}

/* Battle_Init, once the battle's subsystems exist (the fighters' models are loaded by jobs after this). */
void PortMulti_BattleStart(void) {
    if (!on()) {
        return;
    }
    sHeapStart = sHeapMin = Heap_GetFreeSize();
    sSimSum = sSimMax = sRestSum = sRestMax = sFrames = 0;
    sMark = 0;
}

/* Battle_Loop: 0 at the top of a frame, 1 after its simulation, 2 at its end. */
void PortMulti_FrameMark(int where) {
    unsigned long long t, d;
    int free;

    if (!on()) {
        return;
    }
    t = now_ns();
    if (where == 0) {
        sMark = t;
        sSlept = gPortSleptNs;
        return;
    }
    if (sMark == 0) {
        return;
    }
    d = t - sMark;
    if (where == 1) {
        sSimSum += d;
        sSimMax = d > sSimMax ? d : sSimMax;
        sMark = t;
        return;
    }
    if (sHeapCheck > 0) {
        heap_check(sFrames);
    }
    d -= gPortSleptNs - sSlept; /* (the wait for the vertical blank is not work) */
    sRestSum += d;
    sRestMax = d > sRestMax ? d : sRestMax;
    sFrames++;
    sMark = 0;
    free = Heap_GetFreeSize();
    sHeapMin = free < sHeapMin ? free : sHeapMin;
}

/* Battle_Term, before anything is freed. */
void PortMulti_BattleEnd(void) {
    extern int BattleResult_GetReason(void);     /* src/battle/battle_load.c: BTL_REASON_* (battle/battle.h) */
    extern int BattleResult_GetWinnerSide(void);

    if (sHeapCheck > 0) {
        heap_who();
    }
    if (!on() || sFrames == 0) {
        return;
    }
    printf("bt3: mfstat: result reason 0x%X, winner side %d\n", BattleResult_GetReason(), BattleResult_GetWinnerSide());
    {
        extern int BtlPool_PortPeak(int slot), BtlPool_PortOver(int slot), BtlPool_PortSize(int slot); /* btl_pool.c */
        int s;

        printf("bt3: mfstat: pool arenas (most held/size):");
        for (s = 0; s < 9; s++) {
            printf(" %d:%X/%X", s, BtlPool_PortPeak(s), BtlPool_PortSize(s));
            if (BtlPool_PortOver(s)) {
                printf(" OVER by %X", BtlPool_PortOver(s));
            }
        }
        printf("\n");
    }
    printf("bt3: mfstat: fighters %d, heap free %d at setup, least %d (%d used in the battle), %llu frames, "
           "simulation us avg %llu max %llu, rest us avg %llu max %llu\n",
           BtlChar_GetCount(), sHeapStart, sHeapMin, sHeapStart - sHeapMin, sFrames, sSimSum / sFrames / 1000,
           sSimMax / 1000, sRestSum / sFrames / 1000, sRestMax / 1000);
    fflush(stdout);
}
