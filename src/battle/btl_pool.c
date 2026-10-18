#include "common.h"
#include "battle/btl_pool.h"
#include "sys/heap.h"

extern void *memset(void *dst, s32 c, u32 n);

extern BtlPool *gBtlPool;
/* Written by BtlPool_Init and BtlPool_SetInitArg, never read. */
extern s32 gBtlPoolInitArg;

/* The arena block is reached through a pointer to the field, not as a plain member: after a store to it
 * the compiler reloads gBtlPool, which a direct `gBtlPool->arenaMem = x` does not reproduce. */
#define ARENA_MEM (*&gBtlPool->arenaMem)

s32 BtlPool_CalcArenaSize(void);
void BtlPool_CarveArenas(void);
u8 *BtlPool_InitSlot(BtlPoolSlot *slot, s32 size, u8 *mem);

#ifdef PORT
/* Several fighters on the field: the arenas' capacity was never checked, and per-character effect tables grow with
   the fighters. Per arena, the most it held and what did not fit (from the heap instead, see BtlPool_Alloc).
   Game state like the pool itself. Read by port/src/multi_fighter.c (BT3_MFSTAT). */
static s32 sPortPoolPeak[BTL_POOL_SLOT_COUNT];
static s32 sPortPoolOver[BTL_POOL_SLOT_COUNT];

s32 BtlPool_PortPeak(s32 slot) {
    return sPortPoolPeak[slot];
}

s32 BtlPool_PortOver(s32 slot) {
    return sPortPoolOver[slot];
}

s32 BtlPool_PortSize(s32 slot) {
    return gBtlPool != NULL ? gBtlPool->slots[slot].size : 0;
}
#endif

#ifdef PORT
/* What one fighter beyond the two adds to each arena, from BT3_MFSTAT's "most held" of a 1v1 and a 2v1 demo with
   about half again for margin (slot 1: +0x9A00 measured, 3: +0x600, 8: +0x14EC0, 0 and 6: a little; the others
   did not grow). Slot 3 holds a block that grows with the fighters too (0x3480 in a 5v5, more than 0x400 each
   gave: it went to the heap, never given back): 0x500 each measured, 0x800 given. A 1v1 gets the console's sizes. */
static const s32 kPortArenaPerFighter[9] = {0x400, 0x10000, 0, 0x800, 0, 0, 0x100, 0, 0};

static s32 BtlPool_PortFighters(void) {
    extern s32 BattleSide_GetCount(void); /* battle_load.c: the battle's fighters */

    return BattleSide_GetCount();
}

static s32 BtlPool_PortArenaSize(s32 slot, s32 size) {
    return size + (BtlPool_PortFighters() - 2) * kPortArenaPerFighter[slot];
}

/* The pool of a character's shots (kind 0) or character effects (kind 1): 4 / 7 for character 0 and 5 / 8 for
   character 1, as the console has them; a pair of its own for each fighter beyond the two. */
s32 BtlPool_PortCharPool(s32 chr, s32 kind) {
    if (chr < 2 || chr >= BTL_FIGHTER_MAX) {
        return kind == 0 ? (chr == 0 ? 4 : 5) : (chr == 0 ? 7 : 8);
    }
    return BTL_POOL_SLOT_EXTRA + (chr - 2) * 2 + kind;
}

void BtlPool_PortResetExtra(s32 kind) {
    s32 k;

    for (k = 2; k < BtlPool_PortFighters(); k++) {
        BtlPool_Reset(BtlPool_PortCharPool(k, kind));
    }
}

/* Size of an arena of a fighter beyond the two: as large as the leads' own of that kind (0x19000 both). */
#define PORT_EXTRA_ARENA 0x19000
#define PORT_ARENA(slot, size) BtlPool_PortArenaSize(slot, size)
#else
#define PORT_ARENA(slot, size) (size)
#endif

/* Allocates the pool manager and the memory behind every arena slot. */
void BtlPool_Init(s32 arg) {
    s32 size;

    gBtlPoolInitArg = arg;
#ifdef PORT
    memset(sPortPoolPeak, 0, sizeof(sPortPoolPeak));
    memset(sPortPoolOver, 0, sizeof(sPortPoolOver));
#endif
    gBtlPool = Heap_Alloc(sizeof(BtlPool), 0x20, 0, HEAP_ANY);
    memset(gBtlPool, 0, sizeof(BtlPool));
    gBtlPool->arenaMask = 0x3FE;
#ifdef PORT
    {
        s32 k;

        for (k = 2; k < BtlPool_PortFighters(); k++) {
            gBtlPool->arenaMask |= BTL_POOL_ARENA_BIT(BtlPool_PortCharPool(k, 0)) | BTL_POOL_ARENA_BIT(BtlPool_PortCharPool(k, 1));
        }
    }
#endif
    size = BtlPool_CalcArenaSize();
    if (size > 0) {
        ARENA_MEM = Heap_Alloc(size, 0x20, 0, HEAP_ANY);
        memset(ARENA_MEM, 0, size);
        BtlPool_CarveArenas();
    }
}

/* Frees the arena memory and the pool manager. */
void BtlPool_Term(void) {
    if (ARENA_MEM != NULL) {
        Heap_Free(ARENA_MEM);
        ARENA_MEM = NULL;
    }
    if (gBtlPool != NULL) {
        Heap_Free(gBtlPool);
        gBtlPool = NULL;
    }
}

/* Allocates size bytes (rounded up to 32) from a slot: bump allocation for an arena, the heap otherwise. */
void *BtlPool_Alloc(s32 slot, s32 size) {
    s32 aligned = BtlPool_AlignUp32(size);
    BtlPoolSlot *s = &gBtlPool->slots[slot];
    void *ptr;

    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(slot)) {
#ifdef PORT
        if (s->used + aligned > s->size) {
            /* the arena is full (the console never checked; with several fighters it can be): from the heap rather
               than over the next arena. Such a block is not given back (arena slots free nothing), so the arenas
               are sized for the fighters (BtlPool_PortArenaSize) and this stays a safety net that BT3_MFSTAT names. */
            sPortPoolOver[slot] += aligned;
            return Heap_Alloc(size, 0x20, 0, HEAP_ANY);
        }
#endif
        ptr = s->cur;
        s->cur += aligned;
        s->used += aligned;
#ifdef PORT
        if (s->used > sPortPoolPeak[slot]) {
            sPortPoolPeak[slot] = s->used;
        }
#endif
    } else {
        s->used += aligned;
        ptr = Heap_Alloc(size, 0x20, 0, HEAP_ANY);
    }
    return ptr;
}

/* Frees a pointer from a slot; a no-op for arena slots, which are only released by BtlPool_Reset. */
void BtlPool_Free(s32 slot, void *ptr) {
    if (!(gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(slot))) {
        if (ptr != NULL) {
            Heap_Free(ptr);
        }
    }
}

/* Rewinds an arena slot to empty. */
void BtlPool_Reset(s32 slot) {
    BtlPoolSlot *s = &gBtlPool->slots[slot];

    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(slot)) {
        s->cur = s->base;
        s->used = 0;
    }
}

/* Selects the slot that BtlPool_GetCurrent reports. */
void BtlPool_SetCurrent(s32 slot) {
    gBtlPool->current = slot;
}

/* Returns the slot selected by BtlPool_SetCurrent. */
s32 BtlPool_GetCurrent(void) {
    return gBtlPool->current;
}

/* Stores the value BtlPool_Init also records (never read back). */
void BtlPool_SetInitArg(s32 arg) {
    gBtlPoolInitArg = arg;
}

/* Returns the total size of all enabled arena slots. */
s32 BtlPool_CalcArenaSize(void) {
    s32 total = 0;

    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(0)) {
        total = BtlPool_AlignUp32(PORT_ARENA(0, 0xA000));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(1)) {
        total += BtlPool_AlignUp32(PORT_ARENA(1, 0x113000));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(2)) {
        total += BtlPool_AlignUp32(PORT_ARENA(2, 0x1E000));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(3)) {
        total += BtlPool_AlignUp32(PORT_ARENA(3, 0x1400));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(4)) {
        total += BtlPool_AlignUp32(PORT_ARENA(4, 0x19000));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(5)) {
        total += BtlPool_AlignUp32(PORT_ARENA(5, 0x19000));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(6)) {
        total += BtlPool_AlignUp32(PORT_ARENA(6, 0x400));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(7)) {
        total += BtlPool_AlignUp32(PORT_ARENA(7, 0x19000));
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(8)) {
        total += BtlPool_AlignUp32(PORT_ARENA(8, 0x19000));
    }
#ifdef PORT
    {
        s32 k;

        for (k = 2; k < BtlPool_PortFighters(); k++) {
            total += 2 * BtlPool_AlignUp32(PORT_EXTRA_ARENA);
        }
    }
#endif
    return total;
}

/* Hands each enabled arena slot its piece of arenaMem. Slots 5 and 8 test the bit of slots 4 and 7 (original bug). */
void BtlPool_CarveArenas(void) {
    u8 *mem = gBtlPool->arenaMem;

    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(0)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[0], BtlPool_AlignUp32(PORT_ARENA(0, 0xA000)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(1)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[1], BtlPool_AlignUp32(PORT_ARENA(1, 0x113000)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(2)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[2], BtlPool_AlignUp32(PORT_ARENA(2, 0x1E000)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(3)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[3], BtlPool_AlignUp32(PORT_ARENA(3, 0x1400)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(4)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[4], BtlPool_AlignUp32(PORT_ARENA(4, 0x19000)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(4)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[5], BtlPool_AlignUp32(PORT_ARENA(5, 0x19000)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(6)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[6], BtlPool_AlignUp32(PORT_ARENA(6, 0x400)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(7)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[7], BtlPool_AlignUp32(PORT_ARENA(7, 0x19000)), mem);
    }
    if (gBtlPool->arenaMask & BTL_POOL_ARENA_BIT(7)) {
        mem = BtlPool_InitSlot(&gBtlPool->slots[8], BtlPool_AlignUp32(PORT_ARENA(8, 0x19000)), mem);
    }
#ifdef PORT
    {
        s32 k;

        for (k = 2; k < BtlPool_PortFighters(); k++) {
            mem = BtlPool_InitSlot(&gBtlPool->slots[BtlPool_PortCharPool(k, 0)], BtlPool_AlignUp32(PORT_EXTRA_ARENA), mem);
            mem = BtlPool_InitSlot(&gBtlPool->slots[BtlPool_PortCharPool(k, 1)], BtlPool_AlignUp32(PORT_EXTRA_ARENA), mem);
        }
    }
#endif
}

/* Points a slot at its arena memory and returns the address just past it. */
u8 *BtlPool_InitSlot(BtlPoolSlot *slot, s32 size, u8 *mem) {
    slot->size = size;
    slot->base = mem;
    slot->cur = mem;
    return mem + size;
}

/* Rounds a size up to a multiple of 32. */
s32 BtlPool_AlignUp32(s32 size) {
    if (size % 32) {
        size = size / 32 * 32 + 32;
    }
    return size;
}
