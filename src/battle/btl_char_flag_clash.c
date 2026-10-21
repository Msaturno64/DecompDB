#include "common.h"
#include "battle/btl_char_flag.h"

/*
 * Clash sequences between the two fighters: 0x1D8750..0x1D9A20.
 *
 * BtlClash_Update runs once per frame after the per-fighter passes. While roster +0x40 is 0 it looks
 * for both fighters being in a clash action at once and starts the matching sequence:
 *   A (states 1..5)   both in actions 0x130..0x132
 *   B (states 6..8)   both in action 0xFA
 *   C (states 9..11)  both in action 0xFB (then 0xFC)
 * The sequences drive the fighter camera, voices and the flags that tell the action code who won.
 */

extern f32 atan2f(f32 y, f32 x);
extern f32 sinf(f32 x);
extern f32 cosf(f32 x);
extern f32 Mathf_Asin(f32 x);
extern void Vec3_Normalize(Vec4 *dst, Vec4 *src);
extern void Vec4_Lerp(Vec4 *dst, Vec4 *a, Vec4 *b, f32 t); /* a + (b - a) * t */
extern f32 BtlUtil_WrapAngle(f32 a);
extern f32 BtlUtil_ClampF(f32 v, f32 lo, f32 hi);
extern BtlFlagChr *BtlChar_Get(s32 i);
extern BtlFlagPose *BtlChar_GetPos(BtlFlagChr *chr);
extern s32 BtlChar_FrameMod(s32 n);
extern u32 BtlChar_Rand(void);
extern void BtlChar_PlayVoice(BtlFlagChr *chr, s32 kind);
extern s32 Battle_GetMode(void);
extern void ChrCam_SetCut(BtlFlagChr *chr, Vec4 *vecA, Vec4 *vecADelta, Vec4 *vecB, Vec4 *vecBDelta, Vec4 *vecC,
                          Vec4 *vecCDelta, s32 unk88, f32 valA, f32 valADelta, f32 valB, f32 valBDelta, f32 valC,
                          f32 valCDelta, s32 unk8C, s32 unk90, s32 unk94, s32 time, s32 flags);
extern void ChrCam_RequestCut(BtlFlagChr *chr, s32 arg1, s32 arg2);
extern void ChrCam_EndCut(BtlFlagChr *chr);
extern s32 BtlCharApi_IsInClashA(s32 objId);                                  /* action id in 0x130..0x132 */
extern s32 BtlAct_GetCurrent(BtlFlagChr *chr);                            /* action id */
extern s32 BtlAct_GetCurrentClass(BtlFlagChr *chr);                            /* class of the technique in use (0..4), or -1 */
extern void BtlAct_CountAndMarkOpponent(BtlFlagChr *chr);                           /* chr->clashCountB++, opponent +0xD4C = 1 */
extern f32 BtlCharApi_GetHeight(s32 objId);                                  /* height */
extern void BtlCharApi_GetNodePos(s32 objId, s32 node, Vec4 *out);            /* world position of a model node */
extern f32 EftStruggle_GetCamDist(s32 side, s32 type);
extern f32 EftStruggle_GetMidDist(void);
extern void EftStruggle_End(s32 objId);
extern s32 BtlStage_GetPathCount(void);                                       /* number of stage paths */
extern BtlClashPath *BtlStage_GetPath(s32 n);                            /* stage path n */
extern s32 BtlSuper_IsThrow(BtlFlagChr *chr, s32 cls);
extern s32 BtlSuper_GetFlags(BtlFlagChr *chr, s32 cls);                   /* attribute word of the technique */
extern s32 BtlSuper_GetDamage(BtlFlagChr *chr, s32 cls, s32 arg2, s32 arg3); /* damage of the technique */
extern void BtlColl_StartThrow(BtlFlagChr *chr, BtlFlagChr *target, s32 cls, s32 attr);
extern void BtlMember_Damage(BtlFlagChr *chr, s32 damage, s32 flags);    /* applies damage */

extern BtlFlagRoster *gBtlChars;
extern Vec4 D_002EC2A0; /* zero vector */

typedef struct BtlClashProgress {
    /* 0x00 */ u8 unk0[0x3C];
    /* 0x3C */ s32 unk3C; /* difficulty */
} BtlClashProgress;
extern BtlClashProgress *gProgress;

#ifdef PORT
/* Several fighters on the field (multi-fighter): a clash is between two fighters set on each other, and roles 0 and
   1 of this file are the fighters sClash[0] and sClash[1] (with two fighters always 0 and 1, as on the console).
   Chosen by BtlClash_Update while no clash runs; game state, as the clash state is. */
extern s32 BattleSide_GetCount(void);         /* battle_load.c */
extern s32 BattleSide_GetTeam(s32 side);
extern s32 BattleSide_GetBand(s32 side); /* battle_load.c: who fights with whom (free for all) */
extern s32 PortMulti_TargetOf(s32 fighter);  /* port/src/multi_fighter.c */
extern s32 PortMulti_InPair(s32 fighter);    /* in a rush sequence or a throw: no clash */
static s32 sClash[2] = {0, 1};
#define CLASH_ID(role) sClash[role]
#define CLASH_CHR(role) BtlChar_Get(sClash[role])

/* The fighter of role 0 / 1 of the clash (eft_r.c: who loses a struggle). */
s32 BtlClash_PortId(s32 role) {
    return sClash[role];
}

/* Beam struggle assists (the user, 2026-10-20, as the Tag Team Mod's author showed them): with more than two
   fighters, a struggle (clash A) is not the two's alone. An enemy of one of them who is not in it may hit that one:
   a blow takes health and does not move it (btl_char_hit.c: armour), a ki attack or a technique feeds the other's
   beam (btl_char_coll_b.c, BtlColl_TryDodge): from then that one's score counts 1.5 times (btl_act_f.c), and what it
   has at once. sAssist: the role so helped, game state as sClash; cleared while no clash runs. */
static s32 sAssist[2];
s32 BtlClash_BothInClashA(void);

/* `victim`'s role (0 / 1) in a running struggle when `attacker` is an enemy of it from outside, else -1. */
s32 BtlClash_PortOutsider(s32 victim, s32 attacker) {
    s32 n = BattleSide_GetCount();
    s32 role;

    if (n <= 2 || gBtlChars == NULL || gBtlChars->clashState < 1 || gBtlChars->clashState > 5 || !BtlClash_BothInClashA()) {
        return -1;
    }
    if (victim == sClash[0]) {
        role = 0;
    } else if (victim == sClash[1]) {
        role = 1;
    } else {
        return -1;
    }
    if (attacker < 0 || attacker >= n || attacker == sClash[0] || attacker == sClash[1] ||
        BattleSide_GetBand(attacker) == BattleSide_GetBand(victim)) {
        return -1;
    }
    return role;
}

/* An outsider's energy attack on the struggler of `role`: the other one is helped. */
void BtlClash_PortAssist(s32 role) {
    s32 helped = role ^ 1;

    if (!sAssist[helped]) {
        BtlFlagChr *chr = CLASH_CHR(helped);

        sAssist[helped] = 1;
        chr->clashCountA += chr->clashCountA / 2;
    }
}

/* Beam assist (as the Tag Team Mod's author's video: "BEAM ASSIST X1.5"): a teammate of a struggler who comes within
   ASSIST_RANGE of it while it struggles joins in beside it: it turns to the other struggler and holds the beam pose of
   the struggle (action 0x130, btl_act_f.c; let go with everyone when it ends), and its teammate is helped as above.
   A computer teammate that is free as the struggle starts (before its lead is counted, from frame 60 of state 2) goes
   there at once, at the struggler's side. sAssister: a bit per fighter so held (game state). */
#define ASSIST_RANGE PortMulti_AssistRange()
extern f32 PortMulti_AssistRange(void); /* port/src/multi_fighter.c: 120, or BT3_TEST_ASSIST_RANGE (tests) */
static u32 sAssister;

static void BtlClash_PortJoin(void) {
    extern void BtlCharApi_GetPos(s32 objId, Vec4 *out);
    extern s32 BtlCharApi_GetHp(s32 objId);
    extern s32 BtlChar_IsFrozen(BtlFlagChr *chr);
    extern void BtlAct_Request(BtlFlagChr *chr, s32 action);
    extern void BtlMove_TurnToPoint(BtlFlagChr *chr, Vec4 *target, f32 yawStep, f32 pitchStep);
    extern s32 BtlCharApi_IsInputInjected(s32 objId); /* btl_char_api.c: the computer plays it */
    s32 n = BattleSide_GetCount();
    s32 early = gBtlChars->clashState == 2 && gBtlChars->clashA.timer < 20;
    s32 k;
    s32 role;

    for (k = 0; k < n && k < 32; k++) {
        s32 act = BtlAct_GetCurrent(BtlChar_Get(k));

        /* (free: alive, not held, standing or moving: the neutral state 0xB and the moves 0xC..0x35) */
        if (k == sClash[0] || k == sClash[1] || ((sAssister >> k) & 1) || BtlCharApi_GetHp(BtlChar_Get(k)->objId) <= 0 ||
            BtlChar_IsFrozen(BtlChar_Get(k)) || act < 0xB || act > 0x35) {
            continue;
        }
        for (role = 0; role < 2; role++) {
            Vec4 me;
            Vec4 ally;
            Vec4 foe;
            f32 dx;
            f32 dy;
            f32 dz;

            if (BattleSide_GetBand(k) != BattleSide_GetBand(sClash[role]) ||
                BattleSide_GetBand(k) == BattleSide_GetBand(sClash[role ^ 1])) {
                continue;
            }
            BtlCharApi_GetPos(BtlChar_Get(k)->objId, &me);
            BtlCharApi_GetPos(CLASH_CHR(role)->objId, &ally);
            dx = me.x - ally.x;
            dy = me.y - ally.y;
            dz = me.z - ally.z;
            BtlCharApi_GetPos(CLASH_CHR(role ^ 1)->objId, &foe);
            if (BtlCharApi_IsInputInjected(BtlChar_Get(k)->objId) && early) {
                /* (the computer, or a player it took over online: beside the struggler, a little behind) */
                Vec4 *at = &BtlChar_GetPos(BtlChar_Get(k))->pos;
                f32 fx = foe.x - ally.x;
                f32 fz = foe.z - ally.z;
                f32 len = __builtin_sqrtf(fx * fx + fz * fz);

                if (len > 1.0f) {
                    fx /= len;
                    fz /= len;
                }
                at->x = ally.x - fz * 35.0f - fx * 12.0f;
                at->y = ally.y;
                at->z = ally.z + fx * 35.0f - fz * 12.0f;
            } else if (dx * dx + dy * dy + dz * dz >= ASSIST_RANGE * ASSIST_RANGE) {
                continue;
            }
            sAssister |= 1u << k;
            BtlClash_PortAssist(role ^ 1); /* (the struggler of `role` is helped) */
            BtlMove_TurnToPoint(BtlChar_Get(k), &foe, 3.14159265f, 3.14159265f);
            BtlAct_Request(BtlChar_Get(k), 0x130);
            break;
        }
    }
}

/* Whether the fighter holds the beam pose beside a struggling teammate. */
s32 BtlClash_PortAssister(s32 fighter) {
    return BattleSide_GetCount() > 2 && fighter >= 0 && fighter < 32 && ((sAssister >> fighter) & 1);
}

/* The band of the struggler that is helped while a struggle runs (gs/ui.cpp writes "BEAM ASSIST X1.5" for it), or
   -1. */
s32 BtlClash_PortAssistBand(void) {
    if (BattleSide_GetCount() <= 2 || gBtlChars == NULL || gBtlChars->clashState < 1 || gBtlChars->clashState > 5) {
        return -1;
    }
    return sAssist[0] ? BattleSide_GetBand(sClash[0]) : sAssist[1] ? BattleSide_GetBand(sClash[1]) : -1;
}

/* Whether the fighter's struggle score counts 1.5 times. */
s32 BtlClash_PortAssisted(s32 fighter) {
    return BattleSide_GetCount() > 2 && ((fighter == sClash[0] && sAssist[0]) || (fighter == sClash[1] && sAssist[1]));
}
#else
#define CLASH_ID(role) (role)
#define CLASH_CHR(role) BtlChar_Get(role)
#endif

/* 1 while both fighters are in actions 0x130..0x132. */
s32 BtlClash_BothInClashA(void) {
    if (BtlCharApi_IsInClashA(CLASH_ID(0)) && BtlCharApi_IsInClashA(CLASH_ID(1))) {
        return 1;
    }
    return 0;
}

/* Raises held flag 0xC3 on a fighter that is in actions 0x130..0x132. */
void BtlClash_HoldClashA(s32 player) {
    if (BtlCharApi_IsInClashA(CLASH_ID(player))) {
        BtlChar_SetHeldFlag(CLASH_CHR(player), 0xC3);
    }
}

/* 1 while both fighters are in action 0xFA. */
s32 BtlClash_BothInActionFA(void) {
    s32 action = BtlAct_GetCurrent(CLASH_CHR(0));

    if (action != 0xFA) {
        return 0;
    }
    return BtlAct_GetCurrent(CLASH_CHR(1)) == action;
}

/* Raises flag 0xBF on a fighter still in action 0xFA (ends it). */
void BtlClash_EndActionFA(s32 player) {
    BtlFlagChr *chr = CLASH_CHR(player);

    if (BtlAct_GetCurrent(chr) == 0xFA) {
        BtlChar_SetFlag(chr, 0xBF);
    }
}

/* 1 while both fighters are in action 0xFB. */
s32 BtlClash_BothInActionFB(void) {
    s32 action = BtlAct_GetCurrent(CLASH_CHR(0));

    if (action != 0xFB) {
        return 0;
    }
    return BtlAct_GetCurrent(CLASH_CHR(1)) == action;
}

/* 1 while both fighters are in action 0xFC. */
s32 BtlClash_BothInActionFC(void) {
    s32 action = BtlAct_GetCurrent(CLASH_CHR(0));

    if (action != 0xFC) {
        return 0;
    }
    return BtlAct_GetCurrent(CLASH_CHR(1)) == action;
}

/* Raises flag 0xC6 on a fighter still in action 0xFC (ends it). */
void BtlClash_EndActionFC(s32 player) {
    BtlFlagChr *chr = CLASH_CHR(player);

    if (BtlAct_GetCurrent(chr) == 0xFC) {
        BtlChar_SetFlag(chr, 0xC6);
    }
}

/* Clash A camera: a one-frame cut orbiting one fighter (type 0 wide, 1 from the side, 2 close on node 0x30). */
void BtlClash_SetOrbitCut(s32 player, s32 type) {
    Vec4 d;
    BtlFlagChr *chr;
    s32 nodeA = 3;
    s32 nodeB = 0x11;
    f32 pitch;
    f32 yaw;
    f32 dist;

    chr = CLASH_CHR(player);
    pitch = 0.0f;
    BtlOpp_GetDelta(chr, &d);
    yaw = 0.0f;
    dist = 0.0f;
    Vec3_Normalize(&d, &d);
    switch (type) {
    case 0:
        if (player == 0) {
            dist = 2.4f;
        } else {
            dist = -2.4f;
        }
        pitch = 0.0f;
        yaw = BtlUtil_WrapAngle(BtlChar_GetPos(chr)->yaw - dist);
        dist = BtlCharApi_GetHeight(chr->objId);
        dist += EftStruggle_GetCamDist(CLASH_ID(player), type);
        break;
    case 1:
        if (player == 0) {
            dist = 0.5f;
        } else {
            dist = -0.5f;
        }
        pitch = 0.0f;
        yaw = BtlUtil_WrapAngle(BtlChar_GetPos(chr)->yaw - dist);
        dist = BtlCharApi_GetHeight(chr->objId);
        dist += EftStruggle_GetCamDist(CLASH_ID(player), type);
        break;
    case 2:
        dist = 3.14159265f;
        nodeA = 0x30;
        nodeB = 0x30;
        yaw = BtlUtil_WrapAngle(BtlChar_GetPos(chr)->yaw - dist);
        pitch = Mathf_Asin(d.y) - 0.2f;
        dist = BtlCharApi_GetHeight(chr->objId) * 0.4f;
        dist += EftStruggle_GetCamDist(CLASH_ID(player), type);
        break;
    }
    ChrCam_SetCut(chr, &D_002EC2A0, &D_002EC2A0, &D_002EC2A0, &D_002EC2A0, &D_002EC2A0, &D_002EC2A0, nodeA, yaw, 0.0f,
                  pitch, 0.0f, dist, 0.0f, nodeA, nodeB, nodeB, 1, 0x45);
}

/* Clash A camera: a one-frame cut around the point between the fighters, swinging with the balance. */
void BtlClash_SetMidCut(Vec4 *mid, f32 bias) {
    Vec4 d;
    Vec4 look;
    Vec4 a;
    Vec4 b;
    BtlFlagChr *chr = CLASH_CHR(0);
    f32 ang = 1.5707963f;
    f32 half;
    f32 base;
    f32 yaw;
    f32 pitch;

    BtlOpp_GetDelta(chr, &d);
    half = 0.5f;
    ang = bias * ang;
    base = 50.0f;
    Vec3_Normalize(&d, &d);
    BtlCharApi_GetNodePos(CLASH_ID(0), 0x11, &a);
    ang = ang * half;
    BtlCharApi_GetNodePos(CLASH_ID(1), 0x11, &b);
    Vec4_Lerp(&look, &b, &a, bias * 0.6f + half);
    ang = ang + -1.5707963f;
    yaw = BtlUtil_WrapAngle(atan2f(d.x, d.z) + ang);
    pitch = -Mathf_Asin(d.y);
    pitch *= bias;
    ChrCam_SetCut(chr, mid, &D_002EC2A0, mid, &D_002EC2A0, &look, &D_002EC2A0, -1, yaw, 0.0f, pitch, 0.0f,
                  EftStruggle_GetMidDist() + base, 0.0f, -1, -1, -1, 1, 0x45);
}

/* Clash C camera: a one-frame cut placed at the first point of a stage path, looking along its angles. */
void BtlClash_SetPathCut(s32 n) {
    Vec4 pos;
    Vec4 look;
    BtlClashPath *path = BtlStage_GetPath(n);

    if (path != NULL) {
        f32 *p = path->points;

        pos.x = p[0];
        pos.y = p[1];
        pos.z = p[2];
        pos.w = 1.0f;
        look.x = pos.x + sinf(p[5]);
        look.y = pos.y - sinf(p[4]);
        look.z = pos.z + cosf(p[5]);
        look.w = 1.0f;
        ChrCam_SetCut(CLASH_CHR(0), &pos, &D_002EC2A0, &pos, &D_002EC2A0, &look, &D_002EC2A0, -1, 0.0f, 0.0f, 0.0f,
                      0.0f, 0.0f, 0.0f, -1, -1, -1, 1, 0x45);
    }
}

/* Squashes the clash balance into -0.8..0.8: x / (|x| + 1) with x = lead / 20. */
f32 BtlClash_CalcBias(s32 lead) {
    f32 x = lead * 0.05f;

    return BtlUtil_ClampF(x / (__builtin_fabsf(x) + 1.0f), -0.8f, 0.8f);
}

/* Clash A, one frame. Returns the next state (0 when it is over). */
s32 BtlClash_UpdateA(s32 state) {
    Vec4 a;
    Vec4 b;
    BtlFlagRoster *roster = gBtlChars;
    BtlFlagChr *chr0 = CLASH_CHR(0);
    BtlFlagChr *chr1 = CLASH_CHR(1);
    s32 count0 = chr0->clashCountA;
    BtlClashA *c = &roster->clashA;
    s32 count1 = chr1->clashCountA;

    switch (state) {
    case 1:
        c->timer = 0;
        c->lead = 0;
        state++;
        BtlCharSnd_PlayStream(chr0, 0x8D30);
        break;
    case 2:
        if (c->timer == 0) {
            BtlChar_PlayVoice(chr0, 0x14);
        }
        if (c->timer == 0x1E) {
            BtlChar_PlayVoice(chr1, 0x14);
        }
        if (c->timer < 0x1E) {
            BtlClash_SetOrbitCut(0, 0);
        } else if (c->timer < 0x3C) {
            BtlClash_SetOrbitCut(1, 0);
        } else {
            if (count1 < count0) {
                c->lead++;
            }
            if (count0 < count1) {
                c->lead--;
            }
            if (c->bias < -0.64f) {
                BtlClash_SetOrbitCut(0, 1);
            } else if (0.64f < c->bias) {
                BtlClash_SetOrbitCut(1, 1);
            } else {
                BtlClash_SetMidCut(&c->mid, c->bias);
            }
        }
        c->timer++;
        if (c->timer >= PORT_BT4(0x6A, 0xA4)) { /* (BT4: REA, 0x1D8FD0) */
            c->timer = 0;
            state++;
        }
        break;
    case 3:
        if (c->timer == 0) {
            if (c->lead == 0) {
                c->lead = BtlChar_FrameMod(2) ? 1 : -1;
            }
            if (c->lead > 0) {
                BtlChar_PlayVoice(chr0, 0x15);
            } else {
                BtlChar_PlayVoice(chr1, 0x15);
            }
        }
        if (c->lead > 0) {
            BtlClash_SetOrbitCut(0, 2);
        } else {
            BtlClash_SetOrbitCut(1, 2);
        }
        c->timer++;
        if (c->timer >= 0x10) {
            c->timer = 0;
            state++;
        }
        break;
    case 4:
        if (c->lead > 0) {
            c->lead += 4;
        } else {
            c->lead -= 4;
        }
        BtlClash_SetMidCut(&c->mid, c->bias);
        if (__builtin_abs(c->lead) >= 0x47) {
            c->timer = 0;
            state++;
        }
        break;
    case 5:
        if (c->timer == 0) {
            BtlFlagChr *winner;
            BtlFlagChr *loser;
            s32 flags = 0;
            s32 cls;
            s32 loserCls;
            s32 damage;

            winner = chr0;
            loser = chr1;
            if (!(c->lead > 0)) {
                if (c->lead < 0) {
                    winner = chr1;
                    loser = chr0;
                } else if (BtlChar_FrameMod(2) == 0) {
                    winner = chr1;
                    loser = chr0;
                }
            }
            cls = BtlAct_GetCurrentClass(winner);
            loserCls = BtlAct_GetCurrentClass(loser);
            BtlChar_SetHeldFlag(winner, 0xC1);
            BtlChar_SetHeldFlag(winner, 0xC3);
            BtlChar_SetHeldFlag(loser, 0xC2);
            if (BtlSuper_IsThrow(winner, cls)) {
                BtlColl_StartThrow(winner, loser, cls, BtlSuper_GetFlags(winner, cls));
                BtlChar_SetHeldFlag(winner, 0xA0);
                EftStruggle_End(winner->objId);
            } else {
                damage = BtlSuper_GetDamage(winner, cls, 0, 1);
                damage += BtlSuper_GetDamage(loser, loserCls, 0, 1) / 2;
#ifdef PORT
                if (BtlClash_PortAssisted(winner->player)) { /* (beam assist: the beam that wins hits 1.5 times as hard) */
                    damage += damage / 2;
                }
#endif
                switch (cls) {
                case 2:
                    flags = 0x800000;
                    break;
                case 3:
                    flags = 0x1000000;
                    break;
                case 4:
                    flags = 0x2000000;
                    break;
                }
                BtlMember_Damage(loser, damage, flags);
                BtlChar_PlayVoice(loser, 0x16);
                BtlCharSnd_PlayStream(loser, 0x8D31);
                EftStruggle_End(-1);
            }
            BtlChar_RaiseFirstClash(winner);
            c->winner = winner->player;
        }
        if (c->winner == CLASH_ID(0)) { /* (the winner is a fighter; roles 0 and 1 are the clash's two) */
            BtlClash_SetOrbitCut(1, 0);
        } else {
            BtlClash_SetOrbitCut(0, 0);
        }
        c->lead = 0;
        state = 0;
        break;
    }
    c->bias = BtlClash_CalcBias(c->lead);
    BtlCharApi_GetNodePos(CLASH_ID(0), 0x11, &a);
    BtlCharApi_GetNodePos(CLASH_ID(1), 0x11, &b);
    Vec4_Lerp(&c->mid, &b, &a, c->bias * 0.5f + 0.5f);
    return state;
}

/* Clash B, one frame. Returns the next state (0 when it is over). */
s32 BtlClash_UpdateB(s32 state) {
    BtlClashB *c = &gBtlChars->clashB;
    BtlFlagChr *chr0 = CLASH_CHR(0);
    BtlFlagChr *chr1 = CLASH_CHR(1);

    switch (state) {
    case 6:
        c->timer = 0;
        state = 7;
        ChrCam_RequestCut(chr0, 1, 6);
        break;
    case 7:
        if (c->timer == 0) {
            BtlChar_PlayVoice(chr0, 0x18);
        }
        if (c->timer == 0xF) {
            BtlChar_PlayVoice(chr1, 0x18);
        }
        c->timer++;
        if (c->timer >= 0x4C) {
            c->timer = 0;
            state = 8;
        }
        if (!BtlClash_BothInActionFA()) {
            state = 0;
            BtlClash_EndActionFA(0);
            BtlClash_EndActionFA(1);
        }
        break;
    case 8:
        if (c->timer == 0) {
            s32 win;
            s32 lose;
            BtlFlagChr *winner;
            BtlFlagChr *loser;

            if (chr1->clashCountB < chr0->clashCountB) {
                win = 0;
            } else if (chr0->clashCountB < chr1->clashCountB) {
                win = 1;
            } else {
                win = BtlChar_FrameMod(2) == 0;
            }
            lose = !win;
            winner = CLASH_CHR(win);
            loser = CLASH_CHR(lose);
            BtlChar_SetFlag(winner, 0xBF);
            BtlChar_SetFlag(loser, 0xC0);
            BtlChar_PlayVoice(winner, 0x19);
            BtlChar_RaiseFirstClash(winner);
            ChrCam_EndCut(chr0);
            c->winner = win;
        }
        c->timer++;
        if (!BtlClash_BothInActionFA()) {
            state = 0;
        }
        break;
    }
    return state;
}

/* Clash C, one frame: every exchange moves the pair to a random point of a random stage path. */
s32 BtlClash_UpdateC(s32 state) {
    BtlClashC *c = &gBtlChars->clashC;
    BtlFlagChr *chr0 = CLASH_CHR(0);
    BtlFlagChr *chr1 = CLASH_CHR(1);
    BtlClashPath *path;
    BtlFlagChr *winner;
    BtlFlagChr *loser;

    switch (state) {
    case 9:
        c->count = 0;
        c->path = (BtlChar_Rand() >> 2) % (u32)(BtlStage_GetPathCount() - 1) + 1;
        c->hold = (BtlChar_Rand() >> 2) % 10 < 7 ? 2 : 1;
        path = BtlStage_GetPath(c->path);
        if (path != NULL) {
            c->point = (BtlChar_Rand() >> 2) % (u32)(path->end - path->first) + path->first;
        } else {
            c->point = 0;
        }
        c->prevPick = -1;
        c->pick = (BtlChar_Rand() >> 2) & 3;
        if (Battle_GetMode() == 1) {
            c->level = gProgress->unk3C;
        } else {
            c->level = PORT_BT4(1, 2); /* (BT4: REA, 0x3BEA28) */
        }
        state++;
        break;
    case 10:
        if (BtlClash_BothInActionFC()) {
            BtlClash_SetPathCut(c->path);
        }
        if (BtlChar_TestFlag(chr0, 0xC4) && BtlChar_TestFlag(chr1, 0xC4)) {
#ifdef PORT
            if (gPortBt4) { /* (BT4: whoever pressed in time, REA 0x26BCC0) */
                if (!*(s32 *)((u8 *)chr0 + 0x1278)) { /* (injected) */
                    if (chr0->unkE58 >= 0) {
                        BtlAct_CountAndMarkOpponent(chr0);
                        if (chr1->unkE58 >= 0) {
                            BtlAct_CountAndMarkOpponent(chr1);
                        }
                    } else if (*(s32 *)((u8 *)chr1 + 0x1278) || chr1->unkE58 >= 0) {
                        BtlAct_CountAndMarkOpponent(chr1);
                    }
                }
            } else
#endif
            if (chr0->unkE58 >= 0) {
                BtlAct_CountAndMarkOpponent(chr0);
            } else {
                BtlAct_CountAndMarkOpponent(chr1);
            }
            c->count++;
#ifdef PORT
            if (gPortBt4 && c->count >= 7 && chr0->clashCountB == chr1->clashCountB) { /* (REA: 0x3BE8E8) */
                BtlChar_SetFlag(chr1, 0xC7);
                state++;
                BtlChar_SetFlag(chr0, 0xC7);
            } else
#endif
            if (c->count >= 7 && chr0->clashCountB != chr1->clashCountB) {
                if (chr1->clashCountB < chr0->clashCountB) {
                    winner = chr0;
                    loser = chr1;
                } else {
                    winner = chr1;
                    loser = chr0;
                }
                BtlChar_SetFlag(winner, 0xC7);
                state++;
                BtlChar_SetFlag(loser, 0xC8);
            } else {
                c->hold--;
                if (c->hold <= 0) {
                    s32 prev = c->path;
                    s32 n = (BtlChar_Rand() >> 2) % (u32)(BtlStage_GetPathCount() - 2);
                    c->path = n + 1;
                    if (c->path >= prev) {
                        c->path = n + 2;
                    }
                    c->hold = (BtlChar_Rand() >> 2) % 10 < 7 ? 2 : 1;
                    c->point = 0;
                    path = BtlStage_GetPath(c->path);
                    if (path != NULL) {
                        c->point = (BtlChar_Rand() >> 2) % (u32)(path->end - path->first) + path->first;
                    }
                } else {
                    path = BtlStage_GetPath(c->path);
                    if (path != NULL) {
                        s32 prev = c->point;
                        c->point = (BtlChar_Rand() >> 2) % (u32)(path->end - path->first - 1) + path->first;
                        if (c->point >= prev) {
                            c->point++;
                        }
                    } else {
                        c->point = 0;
                    }
                }
                c->prevPick = c->pick;
                c->pick = (BtlChar_Rand() >> 2) & 3;
                BtlChar_SetFlag(chr0, 0xC5);
                BtlChar_SetFlag(chr1, 0xC5);
            }
        }
        if (!BtlClash_BothInActionFB() && !BtlClash_BothInActionFC()) {
            state = 0;
            BtlClash_EndActionFC(0);
            BtlClash_EndActionFC(1);
        }
        break;
    case 11:
        if (!BtlClash_BothInActionFC()) {
            state = 0;
        }
        break;
    }
    return state;
}

/* Runs the clash sequences: starts one when both fighters enter a clash action, then steps it. */
#ifdef PORT
/* No clash runs: the first pair of fighters set on each other (lowest indices first) that are both in a clash
   action becomes the clash's two; with none, roles 0 and 1 go back to fighters 0 and 1. */
static s32 BtlClash_PortFindPair(void) {
    s32 n = BattleSide_GetCount();
    s32 i;
    s32 j;

    sAssist[0] = sAssist[1] = 0;
    for (i = 0; i < n && i < 32; i++) { /* (one who assisted is so until out of the pose: never a new struggle's two) */
        if (!BtlCharApi_IsInClashA(BtlChar_Get(i)->objId)) {
            sAssister &= ~(1u << i);
        }
    }

    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            if (PortMulti_TargetOf(i) != j || PortMulti_TargetOf(j) != i || BattleSide_GetBand(i) == BattleSide_GetBand(j) ||
                PortMulti_InPair(i) || PortMulti_InPair(j) || (i < 32 && ((sAssister >> i) & 1)) ||
                (j < 32 && ((sAssister >> j) & 1))) {
                continue;
            }
            sClash[0] = i;
            sClash[1] = j;
            if (BtlClash_BothInClashA() || BtlClash_BothInActionFA() || BtlClash_BothInActionFB()) {
                s32 k;

                for (k = 0; k < n && k < 32; k++) { /* (a last struggle's assisters let go: not this one's) */
                    if ((sAssister >> k) & 1) {
                        BtlChar_SetHeldFlag(BtlChar_Get(k), 0xC3);
                    }
                }
                sAssister = 0;
                return 1;
            }
        }
    }
    sClash[0] = 0;
    sClash[1] = 1;
    return 0; /* (no pair: no clash starts, not even between 0 and 1, who may not be on each other) */
}

/* No clash runs: what state 0 does for the two fighters, for every fighter (none stays in a clash action alone). */
static void BtlClash_PortReleaseAll(void) {
    s32 k;

    for (k = 0; k < BattleSide_GetCount(); k++) {
        if (BtlCharApi_IsInClashA(k)) {
            BtlChar_SetHeldFlag(BtlChar_Get(k), 0xC3);
        }
        if (BtlAct_GetCurrent(BtlChar_Get(k)) == 0xFA) {
            BtlChar_SetFlag(BtlChar_Get(k), 0xBF);
        }
    }
}
#endif

void BtlClash_Update(void) {
    s32 *state = &gBtlChars->clashState;

#ifdef PORT
    if (*state == 0 && (BattleSide_GetCount() <= 2 || BtlClash_PortFindPair())) {
#else
    if (*state == 0) {
#endif
        if (BtlClash_BothInClashA()) {
            *state = 1;
        }
        if (BtlClash_BothInActionFA()) {
            *state = 6;
        }
        if (BtlClash_BothInActionFB()) {
            *state = 9;
        }
    }
    switch (*state) {
    case 0:
#ifdef PORT
        if (BattleSide_GetCount() > 2) {
            BtlClash_PortReleaseAll();
            break;
        }
#endif
        BtlClash_HoldClashA(0);
        BtlClash_HoldClashA(1);
        BtlClash_EndActionFA(0);
        BtlClash_EndActionFA(1);
        break;
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        *state = BtlClash_UpdateA(*state);
#ifdef PORT
        if (BattleSide_GetCount() > 2 && *state >= 2 && *state <= 4) {
            BtlClash_PortJoin();
        }
#endif
        if (!BtlClash_BothInClashA()) {
            BtlClash_HoldClashA(0);
            BtlClash_HoldClashA(1);
            *state = 0;
        }
        break;
    case 6:
    case 7:
    case 8:
        *state = BtlClash_UpdateB(*state);
        break;
    case 9:
    case 10:
    case 11:
        *state = BtlClash_UpdateC(*state);
        break;
    }
}
