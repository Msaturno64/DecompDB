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
#include "multi_fighter.h"

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
/* And every fighter on its own (free for all): decided with it as the battle is set up (PortMulti_WantAll). Game
   state, as gPortMultiAll: both copies of an online match hold the host's. */
int gPortMultiFfa;

int PortMulti_Ffa(void) {
    return gPortMultiAll && gPortMultiFfa;
}

/* Who plays a fighter past the two leads (src/battle/battle_load.c, BattleSetup_PortSpread): the pad of the person
   playing it, or -1 for the computer (as before). BT3_TEAMS_HUMAN=<i>,<j>... (tests): fighters i, j by their own
   pads i, j (pads 2..9, src/sys/pad.c). The online session's players come here too (SPEC-netplay-n.md). */
int PortMulti_PadOf(int fighter) {
    extern int Port_NetActive(void), Port_NetPlayers(void); /* gs/net.c: online, and how many play */
    const char *e = getenv("BT3_TEAMS_HUMAN");

    if (Port_NetActive()) { /* online: player k plays fighter k with pad k; the fighters past the players: the computer */
        return fighter < Port_NetPlayers() ? fighter : -1;
    }

    while (e != NULL && *e != '\0') {
        if (atoi(e) == fighter) {
            return fighter;
        }
        e = strchr(e, ',');
        e = e != NULL ? e + 1 : NULL;
    }
    return -1;
}

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
extern int BattleSide_GetBand(int side); /* who fights with whom (free for all) */
extern int BattleSide_GetTeam(int side);
extern int BattleSide_GetObjId(int side);
extern void BtlCharApi_GetPos(int objId, float *out); /* src/battle/btl_capi_a.c */
extern void GAME_PTR BtlChar_Get(int i);              /* src/battle/btl_char_mgr.c */
extern int BtlChar_IsDead(void *chr);
extern int BtlCharApi_GetAction(int objId);            /* src/battle/btl_capi_b.c */

/* Each fighter's opponent beyond the two: the enemy it is set on. Game state (not PORT_HOST): a rollback brings it
   back with the fighters. -1: none yet. */
static int sTarget[FIGHTER_MAX];
static int sTargetFrame, sL3Prev[FIGHTER_MAX], sHpPrev[FIGHTER_MAX]; /* (see PortMulti_UpdateTargets) */
/* While one hit between two fighters is tested or applied (btl_char_hit.c), each one's opponent is the other one,
   whoever its target is; -1: none. Set and cleared within one call, so it is never left over in a saved state. */
static int sOverride[FIGHTER_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

void PortMulti_SetOverride(int fighter, int opponent) {
    int k;

    if (fighter < 0) { /* every one ends (eft_det_a.c: a pass that can leave one set through a `continue`) */
        for (k = 0; k < FIGHTER_MAX; k++) {
            sOverride[k] = -1;
        }
    } else if (fighter < FIGHTER_MAX) {
        sOverride[fighter] = opponent;
    }
}

/* The fighter each one is in an exchange with, -1: none. The game's hit actions outlast the lock-on, so both ends of
   a hit must agree on the pair (as the Tag Team Mod): a fighter that is hit takes its attacker as opponent while it
   reacts (ENGAGED_ACTION), one that hits with a technique takes its victim while the technique lasts. Before, the
   victim of an extra's rush kept its lock-on as opponent and stayed in the rush's hits after the rush. Game state. */
static int sEngaged[FIGHTER_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1}, sEngagedAt[FIGHTER_MAX];
#define ENGAGE_GRACE 15 /* frames an exchange holds anyway: the reaction starts at the victim's next action update */
/* hit reactions, throws, down and getting up (0xB4..0xEB); techniques and their receiving end (0x105..0x13B) */
#define ENGAGED_ACTION(a) (((a) >= 0xB4 && (a) <= 0xEB) || ((a) >= 0x105 && (a) <= 0x13B))

/* The last enemy whose attack reached each fighter (a hit, or guarded, dodged or countered alike) and when: a
   player's counter that warps behind the opponent goes behind that one and locks on to it (PortMulti_CounterWarp;
   the user's choice: the counter is the player choosing to answer that enemy). */
#define THREAT_FRAMES 30
static int sThreat[FIGHTER_MAX], sThreatAt[FIGHTER_MAX];
/* and the other way round: whom each fighter's attack reached last (BtlChars_UpdateFreeze: whom its freeze holds) */
#define VICTIM_FRAMES 10
static int sVictim[FIGHTER_MAX], sVictimAt[FIGHTER_MAX];

void PortMulti_NoteThreat(int victim, int attacker) {
    int n = BattleSide_GetCount();

    if (n > 2 && victim >= 0 && victim < n && attacker >= 0 && attacker < n &&
        BattleSide_GetBand(victim) != BattleSide_GetBand(attacker)) {
        sThreat[victim] = attacker;
        sThreatAt[victim] = sTargetFrame;
        sVictim[attacker] = victim;
        sVictimAt[attacker] = sTargetFrame;
    }
}

int PortMulti_TargetOf(int i);

/* With more than two fighters a frozen fighter (another's freeze holds it) takes no part in the melee step: body
   contacts, clashes, rushes, hits (btl_char_hit.c, eft_det_a.c), as the console skips the whole step while anyone is
   frozen. Shots still reach it (an ultimate's beam its victim). */
int PortMulti_Frozen(int i) {
    extern int BtlChar_IsFrozen(void *chr);     /* src/battle/btl_char_get.c */
    extern int BtlChange_PortHasChara(int player); /* src/battle/btl_char_ctl_b.c */

    /* (so is one in the middle of a character change: the console's freeze of the field kept everyone off it, and a
       hit that cut a transformation short left its animation tables on the new files: Bardock 2v2, a crash later) */
    return BattleSide_GetCount() > 2 && i >= 0 && i < BattleSide_GetCount() &&
           (BtlChar_IsFrozen((void *)BtlChar_Get(i)) || BtlChange_PortHasChara(i));
}

/* btl_char_mgr.c, BtlChars_UpdateFreeze, with more than two fighters: the one fighter i's freeze (flag 0x125 of a
   technique, 0x126 of a blow) holds instead of the whole field (as the Tag Team Mod's special_pause / hitstop12):
   the enemy its attack reached in the last frames, else its opponent (a rush's or a throw's pair included). */
int PortMulti_FreezeVictim(int i) {
    int n = BattleSide_GetCount();

    if (i < 0 || i >= n) {
        return -1;
    }
    if (sVictim[i] >= 0 && sVictim[i] < n && sTargetFrame - sVictimAt[i] <= VICTIM_FRAMES) {
        return sVictim[i];
    }
    return PortMulti_TargetOf(i);
}

/* btl_char_move.c, BtlMove_WarpBehindOpponent, before it looks at the opponent: a player's counter (action 0x24,
   vanish and reappear behind the opponent; 0x2D, appear behind it and strike) answering an attack of an enemy it is
   not locked on locks on to that enemy, so the warp goes behind it. Not Instant Transmission and the like (they
   warp behind the lock-on). BT3_MF_COUNTERTEST=1 (tests): computer fighters too, with a line in the log. */
int PortMulti_TargetOf(int i);

/* A fighter that keeps its lock-on whoever hits it: a player's (BTL_CONTROL_PAD). BT3_MF_COUNTERTEST=1 (tests):
   every fighter, so that the computer's counters take the players' way. */
static int keeps_lock_on(int i) {
    extern int BattleSide_GetControl(int side); /* src/battle/battle_load.c: 0 a pad, 2 the computer */
    PORT_HOST static int test = -1;

    if (test < 0) {
        test = getenv("BT3_MF_COUNTERTEST") != NULL;
    }
    return BattleSide_GetControl(i) == 0 || test;
}

void PortMulti_CounterWarp(int i) {
    extern int BtlCharApi_GetAction(int objId); /* src/battle/btl_capi_b.c */
    extern int gPortResim;                      /* gs/state.c */
    int n = BattleSide_GetCount(), a, e;

    if (n <= 2 || i < 0 || i >= n || !keeps_lock_on(i)) {
        return;
    }
    a = BtlCharApi_GetAction(BattleSide_GetObjId(i));
    e = sThreat[i];
    if ((a != 0x24 && a != 0x2D) || e < 0 || e >= n || sTargetFrame - sThreatAt[i] > THREAT_FRAMES ||
        e == PortMulti_TargetOf(i) || BattleSide_GetBand(e) == BattleSide_GetBand(i) || BtlChar_IsDead(BtlChar_Get(e))) {
        return;
    }
    if (getenv("BT3_MF_COUNTERTEST") != NULL && !gPortResim) {
        printf("bt3: counter: fighter %d (action %X) warps behind %d (lock-on was %d)\n", i, a, e, PortMulti_TargetOf(i));
    }
    sTarget[i] = e;
}

/* A hit of `attacker` reached `victim` (btl_char_hit.c, eft_det_a.c). A player's fighter is never put in an exchange:
   its opponent is the one it locked on (L3), whoever hits it (the user: being hit by another enemy switched the
   target back and forth, "dizzying"); the hit itself is still resolved against its attacker (PortMulti_SetOverride),
   and a rush or a throw still pairs the two (PortMulti_TargetOf). Computer fighters turn to whoever is on them. */
void PortMulti_Engage(int victim, int attacker) {
    extern int BtlCharApi_IsInTechnique(int objId); /* src/battle/btl_capi_a.c */
    extern int BtlCharApi_GetAction(int objId);     /* src/battle/btl_capi_b.c */
    int n = BattleSide_GetCount(), a;

    if (n <= 2 || victim < 0 || victim >= n || attacker < 0 || attacker >= n ||
        BattleSide_GetBand(victim) == BattleSide_GetBand(attacker)) {
        return;
    }
    PortMulti_NoteThreat(victim, attacker);
    if (!keeps_lock_on(victim)) {
        sEngaged[victim] = attacker;
        sEngagedAt[victim] = sTargetFrame;
    }
    a = BtlCharApi_GetAction(BattleSide_GetObjId(attacker));
    if (!keeps_lock_on(attacker) &&
        (BtlCharApi_IsInTechnique(BattleSide_GetObjId(attacker)) || (a >= 0xB4 && a <= 0xBF))) { /* (and a throw) */
        sEngaged[attacker] = victim;
        sEngagedAt[attacker] = sTargetFrame;
    }
}

/* Whether a hit of `attacker` may reach `victim`. A rush sequence (attacker 0x12D..0x12F, receiving end
   0x139..0x13B) and a throw (thrower and thrown 0xB5..0xBC, the carry pair 0xBE / 0xBF; not the grab dash 0xB4) are
   between their two fighters only: no outside contact reaches either, and neither touches anyone else (as the Tag
   Team Mod's cinematic contact guard; before, a rush also hit the bystanders and its attacker turned to them, and two
   could grab one fighter at once). btl_char_hit.c, eft_det_a.c; btl_char_mgr.c for a freeze (BtlChars_UpdateFreeze). */
#define PAIR_ACTION(a)                                                                                                 \
    (((a) >= 0xB5 && (a) <= 0xBC) || (a) == 0xBE || (a) == 0xBF || ((a) >= 0x12D && (a) <= 0x12F) ||                   \
     ((a) >= 0x139 && (a) <= 0x13B))
int PortMulti_TargetOf(int i);

static int cinema_owner(int *on);

/* The fighter f is in a pair with: its own opponent while it is in a pair action, else one in a pair action with f
   as its opponent (the frame or two between the victim's sequence and the attacker's: an outsider's freeze held the
   attacker there and the pair ran out of step, Goku GT SSJ4 2v5), else -1. */
static int pair_partner(int f) {
    extern int BtlCharApi_GetAction(int objId); /* src/battle/btl_capi_b.c */
    int n = BattleSide_GetCount(), k;

    if (PAIR_ACTION(BtlCharApi_GetAction(BattleSide_GetObjId(f)))) {
        return PortMulti_TargetOf(f);
    }
    for (k = 0; k < n; k++) {
        if (k != f && PAIR_ACTION(BtlCharApi_GetAction(BattleSide_GetObjId(k))) && PortMulti_TargetOf(k) == f) {
            return k;
        }
    }
    return -1;
}

int PortMulti_CanContact(int attacker, int victim) {
    extern int BtlChange_PortHasChara(int player); /* src/battle/btl_char_ctl_b.c */
    int on, owner, p, q;

    if (BattleSide_GetCount() <= 2) {
        return 1;
    }
    if (BtlChange_PortHasChara(attacker) || BtlChange_PortHasChara(victim)) { /* (a character change: PortMulti_Frozen) */
        return 0;
    }
    /* a cinematic's two (its owner and the owner's opponent) touch only each other while it plays, as a rush's pair
       (the Tag Team Mod's cinematic_contact_guard; SPEC-cinematicas.md, C4) */
    owner = cinema_owner(&on);
    if (on && owner >= 0) {
        int other = PortMulti_TargetOf(owner);

        if ((attacker == owner || attacker == other) != (victim == owner || victim == other)) {
            return 0;
        }
    }
    p = pair_partner(attacker);
    q = pair_partner(victim);
    return (p < 0 || p == victim) && (q < 0 || q == attacker);
}

/* Whether the fighter is in a rush sequence or a throw (PAIR_ACTION): btl_char_flag_clash.c pairs no clash with it
   (a beam aimed at someone else met the rush that caught its owner: the clash cut the rush short, and the partner
   object it had asked for was never taken: the change queue, and every load after it, waited for ever). */
int PortMulti_InPair(int fighter) {
    extern int BtlCharApi_GetAction(int objId); /* src/battle/btl_capi_b.c */

    return PAIR_ACTION(BtlCharApi_GetAction(BattleSide_GetObjId(fighter)));
}

/* The fighter a cinematic (the demo camera) is playing on, -1 when none is on or it plays on no fighter (a partner,
   the stage): the object its animation is attached to (gDemoCam + 0x300, + 0x304), read as DemoCam_IsActive tests
   it but without its side effect (it ends a camera asked to stop). */
static int cinema_owner(int *on) {
    extern char GAME_PTR gDemoCam;              /* src/battle/btl_demo_cam.c (battle/btl_demo_cam.h) */
    extern void GAME_PTR BtlObj_Get(int id);    /* src/battle/btl_obj.c */
    void GAME_PTR obj;
    void GAME_PTR chr;
    int n = BattleSide_GetCount(), flags, i;

    *on = 0;
    if (gDemoCam == NULL) {
        return -1;
    }
    flags = *(int GAME_PTR)(gDemoCam + 0x308); /* DEMO_CAM_PLAYING 1, DEMO_CAM_STOP 2 */
    *on = *(int GAME_PTR)(gDemoCam + 0x32C) != 0 || /* (fixed) */
          (*(int GAME_PTR)(gDemoCam + 0x2C0) != 0 && (flags & 1) && !(flags & 2)); /* (an animation playing) */
    obj = *(void GAME_PTR GAME_PTR)(gDemoCam + 0x300);
    chr = *(void GAME_PTR GAME_PTR)(gDemoCam + 0x304);
    for (i = 0; *on && i < n; i++) {
        void GAME_PTR o = BtlObj_Get(BattleSide_GetObjId(i));

        if (o != NULL && (o == obj || o == chr)) {
            return i;
        }
    }
    return -1;
}

/* -------------------------------------------------------------- whose technique camera the player sees (C5) */

/* The fighter each side's view shows (btl_cam.c, BtlCam_UpdateView: the lead, or a teammate whose technique camera
   is higher). Only for choosing the view drawn (PORT_HOST). */
PORT_HOST static int sViewSubject[2] = {0, 1};

void PortMulti_NoteViewSubject(int view, int fighter) {
    if (view >= 0 && view < 2) {
        sViewSubject[view] = fighter;
    }
}

/* The fighter whose camera the screen is: the local player's online (its view), else the one player's fighter when
   one side is a player's and the other the computer's; -1 with two players on one screen or none (the demo). */
static int viewer(void) {
    extern int BattleSide_GetControl(int side); /* src/battle/battle_load.c: 0 a pad, 2 the computer */
    extern int Port_NetActive(void), Port_NetView(void), Port_NetViewFighter(void); /* gs/net.c */

    if (Port_NetActive()) {
        return Port_NetViewFighter() >= 0 ? Port_NetViewFighter() : Port_NetView();
    }
    if (BattleSide_GetControl(0) == 0 && BattleSide_GetControl(1) != 0) {
        return 0;
    }
    if (BattleSide_GetControl(1) == 0 && BattleSide_GetControl(0) != 0) {
        return 1;
    }
    return -1;
}

/* btl_cam.c, BtlCam_UpdateOverride (as the Tag Team Mod's cinematic_camera_state / special_camera_arbitration):
   whether the screen shows a technique camera of `subject` (a view's priority, or the demo camera's cinematic): with
   more than two fighters only when the player is in it, its owner or the owner's opponent; else the player keeps its
   own camera and the technique plays on the stage. Unknown subject (a partner's, the stage's) or no single player:
   shown, as on the console. */
int PortMulti_ViewerSees(int subject) {
    int v = viewer();

    if (BattleSide_GetCount() <= 2 || v < 0 || subject < 0 || subject >= BattleSide_GetCount()) {
        return 1;
    }
    return subject == v || PortMulti_TargetOf(subject) == v;
}

/* What BtlCam_UpdateOverride chose this frame, for BT3_MFLOG: the view drawn, a cinematic on, and shown. */
PORT_HOST static int sShownView = -1, sShownCinema, sShownCinemaSeen;

void PortMulti_NoteShown(int view, int cinema, int shown) {
    sShownView = view;
    sShownCinema = cinema;
    sShownCinemaSeen = shown;
}

int PortMulti_ViewSubject(int view) {
    return view >= 0 && view < 2 ? sViewSubject[view] : -1;
}

int PortMulti_CinemaOwner(void) {
    int on, owner = cinema_owner(&on);

    return on ? owner : -1;
}

/* btl_act_j.c, at the start of BtlAct_CanUseTechnique, CanUseSkill, QueueComboFinish, CanFuse, QueueFusion and
   QueueSwitch (as the Tag Team Mod's cinematic_admission): with more than two fighters and a cinematic on, a fighter
   that takes no part in it (its owner and the owner's opponent) starts none of them until it ends. One cinematic at a
   time: the demo camera is one, and the fight goes on around it (C1), so a second one took the first's over. */
int PortMulti_CinemaBusy(int fighter) {
    int on, owner;

    if (BattleSide_GetCount() <= 2) {
        return 0;
    }
    owner = cinema_owner(&on);
    if (!on) {
        return 0;
    }
    return owner < 0 || (fighter != owner && fighter != PortMulti_TargetOf(owner));
}

/* BTL_TIME_STOPPED_FOR (battle/btl_multi.h): whether a model change stops this fighter's input, gauges, timers and
   moves. With two fighters every change stops both, as on the console; with more, only the changing fighter and its
   rush or throw pair (the others go on fighting while its model loads; SPEC-cinematicas.md, C2). */
int PortMulti_TimeStoppedFor(int fighter) {
    extern int BtlChars_IsTimeStopped(void); /* src/battle/btl_char_ctl_b.c: a change request is active */
    extern int BtlChange_GetPlayer(void);    /* its fighter, -1 */
    int n = BattleSide_GetCount(), p;

    if (!BtlChars_IsTimeStopped()) {
        return 0;
    }
    if (n <= 2) {
        return 1;
    }
    p = BtlChange_GetPlayer();
    return p == fighter || (p >= 0 && p < n && PortMulti_InPair(p) && PortMulti_TargetOf(p) == fighter);
}

/* While fighter i's camera is worked out (btl_char_mgr.c, around ChrCam_Update), its opponent is its lock-on target
   and not the one it is in an exchange with: being hit by another enemy turned the camera to that one (the user:
   "the target must not change when I am hit"). The rush and throw pair still counts (the cut frames the pair). -1:
   no camera pass. Raised and lowered within one call. */
static int sCamPass = -1;

void PortMulti_CamPass(int fighter) {
    sCamPass = fighter;
}

/* The opponent of fighter i (BTL_OPP, battle/battle.h). With two fighters the other one, as on the console; with
   more the one it is in an exchange with, else the target, or the first enemy while there is none yet (before the
   first PortMulti_UpdateTargets). */
int PortMulti_TargetOf(int i) {
    int n = BattleSide_GetCount(), t, k;

    if (n <= 2 || i < 0 || i >= n) {
        return i ^ 1;
    }
    if (sOverride[i] >= 0) {
        return sOverride[i];
    }
    if (PAIR_ACTION(BtlCharApi_GetAction(BattleSide_GetObjId(i)))) {
        /* in a rush sequence or a throw: the other fighter of its throw block (fighter + 0xE90: the attacker's side at
           + 4, the victim's at + 8; both ends of the pair hold the same), as the Tag Team Mod; the lock-on and the
           last hitter may be someone else (the victim of an extra's rush took its animations from fighter 1's file
           and ran through its steps alone: the attacker waited for ever for a model swap already done) */
        int GAME_PTR thr = (int GAME_PTR)((char GAME_PTR)BtlChar_Get(i) + 0xE90);

        k = thr[1] == i ? thr[2] : thr[2] == i ? thr[1] : -1;
        if (k >= 0 && k < n && BattleSide_GetBand(k) != BattleSide_GetBand(i)) {
            return k;
        }
    }
    if (sEngaged[i] >= 0 && sEngaged[i] < n && sCamPass != i) {
        return sEngaged[i];
    }
    t = sTarget[i];
    if (t < 0 || t >= n || BattleSide_GetBand(t) == BattleSide_GetBand(i)) {
        for (k = 0; k < n && BattleSide_GetBand(k) == BattleSide_GetBand(i); k++) {
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
        sEngaged[i] = -1;
        sL3Prev[i] = 1; /* (an L3 held into the fight is not a tap) */
        sHpPrev[i] = 0x7FFFFFFF;
        sThreat[i] = -1;
        sVictim[i] = -1;
    }
    sTargetFrame = 0;
}

/* The enemy of fighter i that can fight and is nearest to it (the lowest index of equally near ones, so both
   players' copies agree), -1 with none. */
static int nearest_enemy(int i, int n, float p[][4]) {
    int k, best = -1;
    float d, bestD = 0.0f, dx, dy, dz;

    for (k = 0; k < n; k++) {
        if (BattleSide_GetBand(k) == BattleSide_GetBand(i) || BtlChar_IsDead(BtlChar_Get(k))) {
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
    return best;
}

/* The next enemy of fighter i after `from` that can fight, in fighter order, round to the start; -1 with none. */
static int next_enemy(int i, int n, int from) {
    int s, k;

    for (s = 1; s <= n; s++) {
        k = ((from < 0 ? 0 : from) + s) % n;
        if (BattleSide_GetBand(k) != BattleSide_GetBand(i) && !BtlChar_IsDead(BtlChar_Get(k))) {
            return k;
        }
    }
    return -1;
}

/* The enemies of a fighter that are not down, in order, without `skip` (its target, which is tested first): the
   first one after `prev` (-1: from the start), -1 when none is left. eft_det_a.c: what a shot is tested against. */
int PortMulti_NextEnemyAfter(int fighter, int skip, int prev) {
    int e;

    for (e = prev + 1; e < BattleSide_GetCount(); e++) {
        if (e != skip && BattleSide_GetBand(e) != BattleSide_GetBand(fighter) && !BtlChar_IsDead(BtlChar_Get(e))) {
            return e;
        }
    }
    return -1;
}

#define PAD_L3_BIT 0x2         /* PAD_L3, include/sys/pad.h */
#define CHR_RAW_HELD 0x570      /* the fighter's input block: rawHeld, its pad as it was read (battle/btl_input.h) */
#define CPU_REPICK_FRAMES 90    /* a computer fighter looks again for the nearest enemy every 3 seconds */

/* Battle_Loop, once a frame before the AI. A fighter keeps its target while that one can fight; when it cannot
   (fallen), the nearest enemy that can. On top of that:
     - a player's fighter (BTL_CONTROL_PAD) takes the next enemy at a tap of L3 (the pad as the fighter read it last
       frame: what both machines of an online match have);
     - a computer fighter takes the nearest enemy again every CPU_REPICK_FRAMES frames (each at its own frame), and
       at once when it is hurt while another enemy is nearer than its target (it turns to whoever is on it).
   Everything here is game state: a rollback brings it back. */
static void tech_test(void); /* (BT3_TECHTEST, below) */

void PortMulti_UpdateTargets(void) {
    { /* BT3_HEAPWHO_AT=<n> (tests, with BT3_HEAPWHO and BT3_HEAPCHECK): heap_who at the n-th battle frame, for a
         run that does not end its battle (an online test) */
        static int whoAt = -2, whoFrames;
        if (whoAt == -2) {
            whoAt = getenv("BT3_HEAPWHO_AT") != NULL ? atoi(getenv("BT3_HEAPWHO_AT")) : -1;
        }
        if (whoAt >= 0 && ++whoFrames == whoAt) {
            heap_who();
        }
    }
    extern int BattleSide_GetControl(int side); /* src/battle/battle_load.c: 0 a pad, 2 the computer */
    extern int BtlCharApi_GetHp(int objId);
    extern int BtlCharApi_GetAction(int objId); /* src/battle/btl_capi_b.c */
    int n = BattleSide_GetCount(), i, k, l3, hp;
    float p[FIGHTER_MAX][4] __attribute__((aligned(16)));

    if (n <= 2) {
        tech_test(); /* (BT3_TECHTEST with two fighters: the same techniques in a 1v1, for comparison) */
        return;
    }
    sTargetFrame++;
    for (i = 0; i < n; i++) {
        BtlCharApi_GetPos(BattleSide_GetObjId(i), p[i]);
        k = sEngaged[i];
        if (k >= 0 && (k >= n || BtlChar_IsDead(BtlChar_Get(i)) || BtlChar_IsDead(BtlChar_Get(k)) ||
                       (sTargetFrame - sEngagedAt[i] > ENGAGE_GRACE &&
                        !ENGAGED_ACTION(BtlCharApi_GetAction(BattleSide_GetObjId(i)))))) {
            sEngaged[i] = -1; /* the exchange is over: back to the target */
        }
    }
    for (i = 0; i < n; i++) {
        k = sTarget[i];
        if (!(k >= 0 && k < n && BattleSide_GetBand(k) != BattleSide_GetBand(i) && !BtlChar_IsDead(BtlChar_Get(k)))) {
            k = nearest_enemy(i, n, p);
        }
        hp = BtlCharApi_GetHp(BattleSide_GetObjId(i));
        if (BattleSide_GetControl(i) == 0) {
            l3 = (*(unsigned GAME_PTR)((char GAME_PTR)BtlChar_Get(i) + CHR_RAW_HELD) & PAD_L3_BIT) != 0;
            if (l3 && !sL3Prev[i]) {
                int next = next_enemy(i, n, k);
                k = next >= 0 ? next : k;
            }
            sL3Prev[i] = l3;
        } else if ((sTargetFrame + i * 17) % CPU_REPICK_FRAMES == 0 || (hp < sHpPrev[i] && nearest_enemy(i, n, p) != k)) {
            int near = nearest_enemy(i, n, p);
            k = near >= 0 ? near : k;
        }
        sHpPrev[i] = hp;
        if (k >= 0) {
            sTarget[i] = k;
        }
    }
    if (getenv("BT3_MF_TARGET0") != NULL && atoi(getenv("BT3_MF_TARGET0")) < n) { /* (tests: fighter 0's lock-on) */
        sTarget[0] = atoi(getenv("BT3_MF_TARGET0"));
    }
    tech_test();
}

/* ------------------------------------------------------------------------------------------ the HUD's panels */

/* Whose health, ki and stocks each of the two panels shows (as the Tag Team Mod's hud_subject): the watching
   player's panel keeps that player's fighter, the other one shows the fighter it is set on (L3), so an extra's
   health and ki can be seen. Only while the HUD updates or draws (btl_tech_b.c and hud_0.c raise sHudPass around
   Hud_PreUpdate and Hud_Draw): the AI asks the same by-side questions and keeps the side's own fighter. With two
   players, or none (the demo), the panels are the console's. sHudPass is raised and lowered within one frame. */
static int sHudPass;

void PortMulti_HudPass(int on) {
    sHudPass = on;
}

int PortMulti_HudFighter(int side) {
    extern int BattleSide_GetControl(int side); /* src/battle/battle_load.c: 0 a pad, 2 the computer */
    int n = BattleSide_GetCount(), h, me, t;

    extern int Port_NetActive(void); /* gs/net.c */

    if (!sHudPass || n <= 2 || side < 0 || side > 1) {
        return side;
    }
    if (Port_NetActive()) {
        /* (online the panels are the console's on every copy: the HUD is state the copies hold alike, and each
           copy's player is someone else. A panel that showed this copy's target made them differ: the face drawn
           in it moves the effects' textures in video memory (a 2v1 online test), and with five CPU fighters that
           reached the fight and crashed the copies (net_n_test.py --viewof=all, 2026-10-20)) */
        return side;
    } else if (BattleSide_GetControl(0) == 0 && BattleSide_GetControl(1) != 0) {
        me = h = 0;
    } else if (BattleSide_GetControl(1) == 0 && BattleSide_GetControl(0) != 0) {
        me = h = 1;
    } else {
        return side;
    }
    if (side == h) {
        return me;
    }
    t = sTarget[me];
    return t >= 0 && t < n && BattleSide_GetBand(t) != BattleSide_GetBand(me) ? t : side;
}

/* ------------------------------------------------------------------------------- health bars over the fighters */

/* A small health bar over every fighter that no HUD panel shows (the extras, the teammates), as the Tag Team Mod's
   guest_healthbars: Battle_Draw / Battle_DrawSplit call PortMulti_ProjectBars with each view's camera applied, just
   before the view's Ot_Draw, and the head (node 0x11) is projected with that camera (Vu0Screen_StoreMtx +
   Mtx_ProjectInt, as BtlObj's visibility test); kept when it falls inside the view's scissor. Published once per
   frame for the render thread (ui.cpp draws them over the picture: drawing only, the game never reads any of it).
   PORT_HOST; a re-simulated frame (rollback) is not drawn and does not come here. */
PORT_HOST static PortMultiBar sBarBuild[2 * FIGHTER_MAX], sBarShown[2 * FIGHTER_MAX];

/* v by the view's matrix (rows as Vu0Screen_StoreMtx leaves them: the point is x * row0 + y * row1 + z * row2 + w *
   row3, then divided by w), as Mtx_ProjectInt computes it, but in this file's own arithmetic: Mtx_ProjectInt works in
   the vector unit's registers, which are game state, and the draw is not run again in a rollback, so calling it
   here made two online copies differ (seen: a 2v1 online test differed from a blank that moved with the timing). */
static void bar_project(const float *m, const float *v, int *s) {
    float p[4], q;
    int k;

    for (k = 0; k < 4; k++) {
        p[k] = m[k] * v[0] + m[4 + k] * v[1] + m[8 + k] * v[2] + m[12 + k] * v[3];
    }
    q = p[3] != 0.0f ? 1.0f / p[3] : 0.0f;
    s[0] = (int)(p[0] * q);
    s[1] = (int)(p[1] * q);
    s[2] = p[3] > 0.0f ? (int)(p[2] * q) : -1;
}
PORT_HOST static int sBarBuilt, sBarCount;
PORT_HOST static unsigned sBarSeq;

void PortMulti_ProjectBars(int view, int last) {
    extern void Vu0Screen_StoreMtx(float *dst);                        /* the view's world-to-screen matrix */
    extern void BtlCharApi_GetNodePos(int objId, int node, float *out); /* src/battle/btl_capi_a.c */
    extern int BtlCharApi_GetHp(int objId), BtlCharApi_GetHpMax(int objId);
    extern char GAME_PTR gBtlCamView;                                  /* src/battle/btl_cam.c: the view drawn now */
    float m[16] __attribute__((aligned(16))), v[4] __attribute__((aligned(16)));
    int s[4] __attribute__((aligned(16)));
    int n = BattleSide_GetCount(), i, x, y, x0, x1, y0, y1, me, them;

    extern int gPortResim;          /* gs/state.c: blanks being run again (not drawn) */
    extern char GAME_PTR gHud;      /* src/battle/hud_a.c: flags at + 0x30, 1 = the gauges are shown */

    if (gPortResim) {
        return;
    }
    if (view == 0) {
        sBarBuilt = 0;
    }
    if (n > 2 && gBtlCamView != NULL && gHud != NULL && (*(unsigned GAME_PTR)(gHud + 0x30) & 1)) {
        x0 = *(int GAME_PTR)(gBtlCamView + 0x200); /* BtlCamView.scissorX0 .. scissorY1 (battle/btl_cam.h) */
        x1 = *(int GAME_PTR)(gBtlCamView + 0x204);
        y0 = *(int GAME_PTR)(gBtlCamView + 0x208);
        y1 = *(int GAME_PTR)(gBtlCamView + 0x20C);
        sHudPass = 1; /* (whom the two panels show: those need no bar) */
        me = PortMulti_HudFighter(0);
        them = PortMulti_HudFighter(1);
        sHudPass = 0;
        Vu0Screen_StoreMtx(m);
        for (i = 0; i < n && sBarBuilt < 2 * FIGHTER_MAX; i++) {
            int hp = BtlCharApi_GetHp(BattleSide_GetObjId(i));

            if (i == me || i == them || hp <= 0) {
                continue;
            }
            int feet;

            BtlCharApi_GetPos(BattleSide_GetObjId(i), v); /* the feet: how tall the fighter is on this picture */
            v[3] = 1.0f;
            bar_project(m, v, s);
            feet = s[1] - 0x720;
            BtlCharApi_GetNodePos(BattleSide_GetObjId(i), 0x11, v); /* node 0x11: the neck */
            v[3] = 1.0f;
            bar_project(m, v, s);
            x = s[0] - 0x700; /* (the GS offset of a 512 x 448 frame, as BtlObj_UpdateVisibility) */
            y = s[1] - 0x720;
            if (s[2] < 0 || x < x0 || x > x1 || y < y0 || y > y1) {
                continue; /* behind the camera, or outside this view */
            }
            y -= (feet - y) * 7 / 10; /* above the head: the head is about 0.6 of the neck-to-feet height over it */
            sBarBuild[sBarBuilt].x = x / 512.0f;
            sBarBuild[sBarBuilt].y = y / 448.0f;
            sBarBuild[sBarBuilt].hp = hp;
            sBarBuild[sBarBuilt].hpMax = BtlCharApi_GetHpMax(BattleSide_GetObjId(i));
            sBarBuild[sBarBuilt].ally = BattleSide_GetBand(i) == BattleSide_GetBand(me);
            sBarBuilt++;
        }
    }
    if (last && getenv("BT3_BARLOG") != NULL) { /* (tests: the bars, to lay over a BT3_SHOT picture) */
        static unsigned frames;

        frames++;
        for (i = 0; i < sBarBuilt; i++) {
            printf("bt3: bar: draw %u x %.4f y %.4f hp %d/%d ally %d\n", frames, sBarBuild[i].x, sBarBuild[i].y,
                   sBarBuild[i].hp, sBarBuild[i].hpMax, sBarBuild[i].ally);
        }
    }
    if (last) {
        __atomic_add_fetch(&sBarSeq, 1, __ATOMIC_ACQ_REL);
        memcpy(sBarShown, sBarBuild, sizeof(sBarShown));
        sBarCount = sBarBuilt;
        __atomic_add_fetch(&sBarSeq, 1, __ATOMIC_ACQ_REL);
    }
}

/* The render thread's copy of the last frame's bars (ui.cpp); the count. None once they have not been published for
   a while: a fight left halfway (to the main menu) publishes no more, and its last bars stayed over the menus. */
#define BAR_STALE_READS 30 /* (the window is drawn once or a few times a game frame) */

int PortMulti_ReadBars(PortMultiBar *out, int max) {
    static unsigned lastSeq, sameReads;
    unsigned a, b;
    int tries, c;

    a = __atomic_load_n(&sBarSeq, __ATOMIC_ACQUIRE);
    sameReads = a == lastSeq ? sameReads + 1 : 0;
    lastSeq = a;
    if (sameReads > BAR_STALE_READS) {
        return 0;
    }
    for (tries = 0; tries < 64; tries++) {
        a = __atomic_load_n(&sBarSeq, __ATOMIC_ACQUIRE);
        if (a & 1) {
            continue;
        }
        c = sBarCount < max ? sBarCount : max;
        memcpy(out, sBarShown, c * sizeof(*out));
        b = __atomic_load_n(&sBarSeq, __ATOMIC_ACQUIRE);
        if (a == b) {
            return c;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------- BT3_TECHTEST (tests) */

/* BT3_TECHTEST=1 (tests, with BT3_TEAMS=1v2): fighters 0 and 2 take turns at every technique of theirs on each other
   (BtlCtrl_UseTechnique, as a story script does: blast 1 kinds 0 and 1, blast 2 kinds 2 and 3, ultimate 4), twice
   each; everyone's health is kept up. Lines "bt3: techtest: ..." on stdout: each technique with its action, frames
   and the health it took; STUCK when a fighter stays in one technique, reaction or load for 30 s (the game goes on
   but the fight does not: what a player sees as a hang); then the program ends. Off: nothing here runs.
   BT3_TECHTEST=2: the transformations instead: each fighter takes the form of each of its four transformation slots
   in turn (BtlCtrl_Transform; "kind" 10 + slot in the lines), each time followed by its ultimate in that form.
   BT3_TECHTEST_ROUNDS=1: each technique once instead of twice (BT3_TECHTEST=2: no change). */
#define TT_ROUNDS 10         /* uses per fighter: kinds 2, 3, 4, 0, 1, twice (BT3_TECHTEST=2: 4 forms, 4 ultimates) */
#define TT_BUSY(a) (ENGAGED_ACTION(a) || ((a) >= 0xEC && (a) <= 0xF8)) /* (and the character changes) */
#define TT_STUCK 900         /* 30 s */
static const int kTtKind[5] = {2, 3, 4, 0, 1};

static void tech_test(void) {
    extern int BtlCtrl_UseTechnique(int player, int kind); /* src/battle/btl_capi_b.c */
    extern int BtlCtrl_CanAct(int player);
    extern void BtlCtrl_RaiseHp(int player, int member, float percent);
    extern int BtlCharApi_GetAction(int objId);
    extern int BtlCharApi_GetHp(int objId);
    extern char GAME_PTR Battle_GetWork(void); /* src/battle/battle_work.c */
    static int on = -1, frame, turn, used[FIGHTER_MAX], wait, startAt, dmg, startAction, kind, since[FIGHTER_MAX],
        last[FIGHTER_MAX], prevHp[FIGHTER_MAX];
    static int A; /* the first fighter of the pair: 0, or BT3_TECHTEST_A (1: side 1's lead against its own extra) */
    static int uses; /* per fighter, until the test is done */
    extern int BtlChange_IsPendingType0(int player), BtlChange_IsPendingType1(int player); /* btl_capi_b.c */
    extern int BtlChange_IsLoadedFor(int player), BtlChange_IsReady(void); /* btl_char_ctl_b.c, btl_capi_b.c */
    static int loadSince, reported;
    int f, other, a, i, hp, B;

    if (on < 0) {
        on = getenv("BT3_TECHTEST") != NULL && BattleSide_GetCount() >= 2 && BattleSide_GetCount() <= FIGHTER_MAX ? atoi(getenv("BT3_TECHTEST")) : 0;
    }
    if (!on) {
        return;
    }
    frame++;
    B = BattleSide_GetCount() - 1; /* (the other fighter of the test: the extra, or in a 1v1 fighter 1) */
    if (frame == 1) {
        A = getenv("BT3_TECHTEST_A") != NULL && atoi(getenv("BT3_TECHTEST_A")) < B ? atoi(getenv("BT3_TECHTEST_A")) : 0;
        turn = A;
        uses = on == 2 ? 8 : getenv("BT3_TECHTEST_ROUNDS") != NULL && atoi(getenv("BT3_TECHTEST_ROUNDS")) == 1 ? 5 : TT_ROUNDS;
    }
    f = turn;
    other = f == A ? B : A;
    { /* the change queue (models, partners) of every fighter, each time it moves */
        static char was[FIGHTER_MAX * 4 + 2];
        char now[FIGHTER_MAX * 4 + 2];

        now[0] = '0' + BtlChange_IsReady();
        for (i = 0; i < BattleSide_GetCount(); i++) {
            now[1 + i * 4] = ' ';
            now[2 + i * 4] = '0' + BtlChange_IsPendingType0(i);
            now[3 + i * 4] = '0' + BtlChange_IsPendingType1(i);
            now[4 + i * 4] = '0' + BtlChange_IsLoadedFor(i);
        }
        now[1 + i * 4] = '\0';
        if (strcmp(now, was) != 0) {
            printf("bt3: techtest: frame %d change queue: ready %c, pending0/pending1/loaded of each fighter%s\n", frame,
                   now[0], now + 1);
            strcpy(was, now);
        }
    }
    for (i = 0; i < BattleSide_GetCount(); i++) { /* the health a technique takes is counted before nobody is let fall below 95% */
        hp = BtlCharApi_GetHp(BattleSide_GetObjId(i));
        if (i == other && startAt > 0 && hp < prevHp[i]) {
            dmg += prevHp[i] - hp;
        }
        if (hp <= 0) { /* (a technique that takes more than that: the fight is over, not stuck) */
            printf("bt3: techtest: done, %d stuck (fighter %d knocked out by kind %d of fighter %d)\n", reported, i, kind,
                   turn);
            fflush(stdout);
            exit(reported ? 3 : 0);
        }
        BtlCtrl_RaiseHp(i, 0, 95.0f);
        if (BtlCharApi_GetHp(BattleSide_GetObjId(i)) != hp) {
            BtlCtrl_RaiseHp(i, 0, 100.0f);
        }
        prevHp[i] = BtlCharApi_GetHp(BattleSide_GetObjId(i));
    }
    sTarget[A] = B;
    sTarget[B] = A;
    /* stuck: one action (technique, reaction, down) for 30 s, or a model load that does not end */
    for (i = 0; i < BattleSide_GetCount(); i++) {
        a = BtlCharApi_GetAction(BattleSide_GetObjId(i));
        if (a != last[i]) {
            last[i] = a;
            since[i] = frame;
        } else if (TT_BUSY(a) && frame - since[i] == TT_STUCK) {
            extern int BtlChange_GetPlayer(void); /* btl_char_ctl_b.c */
            int GAME_PTR thr = (int GAME_PTR)((char GAME_PTR)BtlChar_Get(i) + 0xE90); /* the pair block */

            printf("bt3: techtest: STUCK fighter %d in action %X for 30 s (kind %d of fighter %d; pair %d -> %d, change "
                   "queue's fighter %d)\n", i, a, kind, turn, thr[1], thr[2], BtlChange_GetPlayer());
            reported++;
        }
    }
    if ((*(unsigned GAME_PTR)(Battle_GetWork() + 0x19F0) & 0x1800) == 0) { /* BattleWork.flags: LOAD_CHARA, _OBJECT */
        loadSince = frame;
    } else if (frame - loadSince == TT_STUCK) {
        printf("bt3: techtest: STUCK loading a model for 30 s (kind %d of fighter %d; BattleWork flags %X)\n", kind, turn,
               *(unsigned GAME_PTR)(Battle_GetWork() + 0x19F0));
        reported++;
    }
    a = BtlCharApi_GetAction(BattleSide_GetObjId(f));
    if (startAt > 0) { /* a technique was asked for */
        if (startAction == 0 && TT_BUSY(a) && (a >= 0x105 || a >= 0xEC)) {
            startAction = a;
        }
        if ((startAction != 0 && BtlCtrl_CanAct(f)) || frame - startAt > 300 + (startAction ? 3 * TT_STUCK : 0)) {
            extern void GAME_PTR BtlObj_Get(int id); /* src/battle/btl_obj.c: NULL for a free object */
            int objs = 0;

            for (i = 0; i < 64; i++) { /* (the battle objects in use; NULL past the last, src/battle/btl_obj.c) */
                objs += BtlObj_Get(i) != NULL;
            }
            printf("bt3: techtest: frame %d fighter %d kind %d: %s action %X, %d frames, %d health of fighter %d, %d "
                   "objects\n", frame, f, kind, startAction ? "ran" : "did not start:", startAction, frame - startAt,
                   dmg, other, objs);
            startAt = 0;
            used[f]++;
            turn = used[A] > used[B] ? B : A;
            wait = frame + 45;
        }
        return;
    }
    if (used[A] >= uses && used[B] >= uses) {
        printf("bt3: techtest: done, %d stuck\n", reported);
        fflush(stdout);
        exit(reported ? 3 : 0);
    }
    if (frame >= wait && BtlCtrl_CanAct(f) && BtlCtrl_CanAct(other)) {
        kind = on == 2 ? (used[f] % 2 ? 4 : 10 + used[f] / 2) : kTtKind[used[f] % 5];
        dmg = 0;
        if (kind >= 10) {
            /* (what a story script's flag 0x116 + slot does, BtlAct_CheckStoryForced: that one is for mode 1 only) */
            extern int BtlParam_GetSlotId(void *chr, int slot);                          /* src/battle/btl_tech_a.c */
            extern int BtlAct_CanTransform(void *chr, unsigned slot, int needBlast, int needAllowed); /* btl_act_j.c */
            extern int BtlAct_QueueTransform(void *chr, int slot);
            int id = BtlParam_GetSlotId(BtlChar_Get(f), kind - 10);

            if (BtlAct_CanTransform(BtlChar_Get(f), kind - 10, 0, 0)) {
                BtlAct_QueueTransform(BtlChar_Get(f), kind - 10);
            } else {
                printf("bt3: techtest: frame %d fighter %d kind %d: no transformation (slot id %d)\n", frame, f, kind, id);
                used[f] += 2; /* (and the ultimate that would follow it) */
                turn = used[A] > used[B] ? B : A;
                return;
            }
        } else {
            BtlCtrl_UseTechnique(f, kind);
        }
        startAt = frame;
        startAction = 0;
    }
}

/* ------------------------------------------------------------------------------------------- BT3_MFLOG (tests) */

/* BT3_MFLOG=<file>: one line per vertical blank of a fight: the blank, then for every fighter its team, health
   and position (x y z as the bits of the floats, hexadecimal), its target, action (hexadecimal) and whether it is in
   a technique: "<blank> ts <time stopped> pv <priority view> | <team> <hp> <x> <y> <z> <target> <action> <skill> |
   ...". A blank
   run again after a rollback is written again; the last line of a blank counts. Not game state (PORT_HOST). */
void PortMulti_BattleEnd(void); /* (below: the measuring part) */

void PortMulti_Trace(unsigned vblank, int battle) {
    extern int BattleSide_GetCount(void);       /* src/battle/battle_load.c */
    extern int BattleSide_GetBand(int side);
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
    /* BT3_MF_STOPAT=<blank> (tests): the program ends there (a versus battle without players waits at its end) */
    if (getenv("BT3_MF_STOPAT") != NULL && vblank >= (unsigned)atoi(getenv("BT3_MF_STOPAT"))) {
        printf("bt3: stopped at vertical blank %u (BT3_MF_STOPAT)\n", vblank);
        PortMulti_BattleEnd(); /* (BT3_MFSTAT's lines, as at the end of a battle) */
        fflush(stdout);
        if (fp != NULL) {
            fclose(fp);
        }
        exit(0);
    }
    if (battle) { /* (BT3_MFSTAT: the blanks each fighter spent frozen) */
        extern void PortMulti_NoteFrozen(void);
        PortMulti_NoteFrozen();
    }
    if (fp == NULL || !battle) {
        return;
    }
    {
        extern int BtlChars_IsTimeStopped(void);  /* src/battle/btl_char_ctl_b.c */
        extern int BtlCam_GetPriorityView(void);  /* src/battle/btl_cam.c: -1 none */

        fprintf(fp, "%u ts %d pv %d sv %d cin %d/%d", vblank, BtlChars_IsTimeStopped() != 0, BtlCam_GetPriorityView(),
                sShownView, sShownCinema, sShownCinemaSeen);
    }
    for (i = 0; i < BattleSide_GetCount(); i++) {
        obj = BattleSide_GetObjId(i);
        BtlCharApi_GetPos(obj, q);
        memcpy(u, q, sizeof(u)); /* (the bits: this file has no floating-point unit to print with) */
        {
            extern int BtlCharApi_GetAction(int objId); /* src/battle/btl_capi_b.c */
            extern int BtlCharApi_IsInSkill(int objId); /* src/battle/btl_capi_a.c */

            fprintf(fp, " | %d %d %08x %08x %08x %d %X %d", BattleSide_GetBand(i), BtlCharApi_GetHp(obj), u[0], u[1], u[2],
                    PortMulti_TargetOf(i), BtlCharApi_GetAction(obj), BtlCharApi_IsInSkill(obj));
        }
    }
    fputc('\n', fp);
    fflush(fp);
}

/* ---------------------------------------------------------------------------------- the setting (F1, Misc) */

/* "Team battle: every member on the field": setting multi_all, off unless set. Read when a battle is set up. */
extern int Port_Setting(const char *name, int def); /* plat_settings.c */
extern void Port_SettingSave(const char *name, int value);
extern void Port_SettingsWrite(void);
PORT_HOST static int sSetting = -1;

int PortMulti_Setting(void) {
    if (sSetting < 0) {
        sSetting = Port_Setting("multi_all", 0) != 0;
    }
    return sSetting;
}

void PortMulti_SettingSet(int on) {
    sSetting = on != 0;
    Port_SettingSave("multi_all", sSetting);
    Port_SettingsWrite();
}

/* "Free for all": setting multi_ffa, off unless set; with "every member on the field", every fighter fights on its own
   (the teams only say which side's result screen the last one standing brings). */
PORT_HOST static int sFfa = -1;

int PortMulti_FfaSetting(void) {
    if (sFfa < 0) {
        sFfa = Port_Setting("multi_ffa", 0) != 0;
    }
    return sFfa;
}

void PortMulti_FfaSettingSet(int on) {
    sFfa = on != 0;
    Port_SettingSave("multi_ffa", sFfa);
    Port_SettingsWrite();
}

/* "Starting health" of each team (settings multi_hp0 / multi_hp1, percent, 100 unless set): every member of the
   team starts with that share of its health, over 100 too (BtlMember_LoadParams raises the maximum with it), for
   uneven teams (multi-fighter T1.9: no automatic handicap, it is a setting). Read when a battle is set up. */
PORT_HOST static int sHandicap[2] = {-1, -1};

int PortMulti_HandicapSetting(int team) {
    static const char *const kName[2] = {"multi_hp0", "multi_hp1"};

    if (team < 0 || team > 1) {
        return 100;
    }
    if (sHandicap[team] < 0) {
        sHandicap[team] = Port_Setting(kName[team], 100);
        sHandicap[team] = sHandicap[team] < 25 ? 25 : sHandicap[team] > 400 ? 400 : sHandicap[team];
    }
    return sHandicap[team];
}

void PortMulti_HandicapSet(int team, int percent) {
    static const char *const kName[2] = {"multi_hp0", "multi_hp1"};

    if (team >= 0 && team <= 1) {
        sHandicap[team] = percent < 25 ? 25 : percent > 400 ? 400 : percent;
        Port_SettingSave(kName[team], sHandicap[team]);
        Port_SettingsWrite();
    }
}

/* battle_load.c, once a battle is set up with every member on the field: a line in the log (a player's report shows
   which battle it was). PORT_HOST: only says. A battle set up again in a rollback is not one more. */
int PortMulti_Handicap(int team);

void PortMulti_NoteSpread(int fighters) {
    extern int gPortResim; /* gs/state.c */

    if (!gPortResim) {
        printf("bt3: team battle: %d fighters on the field%s (team 1: %d%%, team 2: %d%% starting health)\n", fighters,
               PortMulti_Ffa() ? ", free for all" : "", PortMulti_Handicap(0), PortMulti_Handicap(1));
        fflush(stdout);
    }
}

/* The starting health a battle uses (battle_load.c): online the host's (its rules word, gs/net.c), else the
   player's own setting. */
int PortMulti_Handicap(int team) {
    extern int Port_NetActive(void), Port_NetHandicap(int team); /* gs/net.c */

    return Port_NetActive() ? Port_NetHandicap(team) : PortMulti_HandicapSetting(team);
}

/* battle_load.c BattleSetup_PortSpread, as a battle is set up: whether every member goes on the field. The test
   variable decides when it is there (BT3_TEAMS); otherwise the setting, in a versus battle (the game's own team
   battle among them) played on this machine. Online the two copies would have to agree: the host's rules are to
   carry it (multi-fighter T1.9); until then an online match is as ever. */
int PortMulti_WantAll(void) {
    extern int Battle_GetMode(void); /* src/battle/battle_work.c: BTL_MODE_*, 0 versus */
    extern int Port_NetActive(void); /* gs/net.c */

    if (getenv("BT3_TEAMS") == NULL) {
        extern int Port_NetMultiAll(void); /* gs/net.c: the host's, from its rules word (both copies the same) */

        extern int Port_NetFfa(void); /* gs/net.c: the host's */

        gPortMultiAll = Battle_GetMode() == 0 && (Port_NetActive() ? Port_NetMultiAll() : PortMulti_Setting());
        gPortMultiFfa = Port_NetActive() ? Port_NetFfa() : PortMulti_FfaSetting();
    } else {
        gPortMultiFfa = getenv("BT3_TEAMS_FFA") != NULL; /* (tests: the demo's teams, every fighter on its own) */
    }
    return gPortMultiAll;
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

/* BT3_NET_TEAMS=<a>v<b> (tests of online play with N players, research/tests/net_n_test.py): every battle with
   those team sizes, whatever the select gave, so that the players past the two leads have fighters. Every copy is
   given the same, so all set up the same battle. The added members are the demo's characters. Returns the size
   wanted for a side (0: leave it), and the character of its member i. */
int PortMulti_TestTeamSize(int side) {
    const char *e = getenv("BT3_NET_TEAMS");
    int a, b;

    if (e == NULL || sscanf(e, "%dv%d", &a, &b) != 2 || a < 1 || a > 5 || b < 1 || b > 5) {
        return 0;
    }
    return side == 0 ? a : b;
}

int PortMulti_TestChara(int side, int i) {
    return kDemoExtra[side & 1][(i - 1) % 5];
}

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
        /* as Demo_SetupBattle: CPU, pad = side. BT3_TEAMS_PAD=1 (tests): side 0 is a player's (no buttons pressed),
           for what only a player's fighter has: its HUD panel, its lock-on */
        BattleSetup_SetSide(side, side == 0 && getenv("BT3_TEAMS_PAD") != NULL ? 0 : 2, side, count[side], 1, 1, 0, NULL);
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
    /* BT3_TEAMS_CHARS=<id>,<id>.../<id>,<id>... (tests): the characters of side 0's members, then side 1's (ids
       0..160, the game's character numbers); a member not listed keeps the demo's */
    if (getenv("BT3_TEAMS_CHARS") != NULL) {
        const char *e = getenv("BT3_TEAMS_CHARS");

        for (side = 0; side < 2 && e != NULL; side++) {
            for (i = 0; i < count[side] && *e != '\0' && *e != '/'; i++) {
                BattleSide_GetMember(side, i)->word[0] = atoi(e);
                e += strcspn(e, ",/");
                e += *e == ',';
            }
            e = strchr(e, '/');
            e = e != NULL ? e + 1 : NULL;
        }
    }
    /* BT3_TEAMS_CPU=<level> (tests): every member's computer level, 0..29 (the demo's is low: few techniques) */
    if (getenv("BT3_TEAMS_CPU") != NULL) {
        for (side = 0; side < 2; side++) {
            for (i = 0; i < count[side]; i++) {
                BattleSide_GetMember(side, i)->word[3] = atoi(getenv("BT3_TEAMS_CPU")); /* BattleMember.cpuLevel */
            }
        }
    }
    /* BT3_TEAMS_MODE=<n> (tests): the battle's mode (BattleRule.mode, setup + 8); 0 makes the attract demo a versus
       battle, where the computer fighters use their whole move set (the demo's mode 7 keeps to a few moves) */
    if (getenv("BT3_TEAMS_MODE") != NULL) {
        extern int GAME_PTR Battle_GetSetup(void); /* src/battle/battle_work.c: the setup's words */
        Battle_GetSetup()[2] = atoi(getenv("BT3_TEAMS_MODE"));
    }
    /* BT3_TEAMS_TIME=<n> (tests): the time limit (BattleRule.timeLimit, setup + 0x10): 0 none, 1..5 60..45 s */
    if (getenv("BT3_TEAMS_TIME") != NULL) {
        extern int GAME_PTR Battle_GetSetup(void); /* src/battle/battle_work.c */
        Battle_GetSetup()[4] = atoi(getenv("BT3_TEAMS_TIME"));
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
/* btl_char_hit.c, BtlColl_Update: an ordinary attack's hit that took health (path 0: on the attacker's target, the
   console's way; 1: on another enemy it touched, BtlHit_PortExtraContacts). Counted for BT3_MFSTAT; only says. */
PORT_HOST static int sMelee[2][FIGHTER_MAX];
PORT_HOST static int sFrozen[FIGHTER_MAX], sFrozenAll, sFrozenBlanks;

/* PortMulti_Trace, each fight blank: who is frozen (held by another's freeze), and whether everyone but one is. */
void PortMulti_NoteFrozen(void) {
    extern int BtlChar_IsFrozen(void *chr); /* src/battle/btl_char_get.c */
    int n = BtlChar_GetCount(), i, k = 0;

    for (i = 0; i < n && i < FIGHTER_MAX; i++) {
        if (PortMulti_Frozen(i) || (n <= 2 && BtlChar_IsFrozen((void *)BtlChar_Get(i)))) {
            sFrozen[i]++;
            k++;
        }
    }
    sFrozenAll += n > 2 && k >= n - 1;
    sFrozenBlanks++;
}

void PortMulti_NoteMelee(int path, int victim) {
    if (victim >= 0 && victim < FIGHTER_MAX) {
        sMelee[path != 0][victim]++;
    }
}

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
        int i;

        printf("bt3: mfstat: blanks frozen by another's freeze, by fighter (of %d; the whole field but one: %d):",
               sFrozenBlanks, sFrozenAll);
        for (i = 0; i < BtlChar_GetCount(); i++) {
            printf(" %d:%d", i, sFrozen[i]);
        }
        printf("\n");
        printf("bt3: mfstat: ordinary hits that took health, by victim (as the target / as another enemy touched):");
        for (i = 0; i < BtlChar_GetCount(); i++) {
            printf(" %d:%d/%d", i, sMelee[0][i], sMelee[1][i]);
        }
        printf("\n");
    }
    {
        extern int BtlPool_PortPeak(int slot), BtlPool_PortOver(int slot), BtlPool_PortSize(int slot); /* btl_pool.c */
        int s;

        printf("bt3: mfstat: pool arenas (most held/size):");
        for (s = 0; s < 25; s++) { /* BTL_POOL_SLOT_COUNT: the console's nine, then the extra fighters' pairs */
            if (BtlPool_PortSize(s) == 0) {
                continue;
            }
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
