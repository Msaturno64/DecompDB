#ifndef BATTLE_BTL_MULTI_H
#define BATTLE_BTL_MULTI_H

#include "types.h"

/*
 * Several fighters on the field at once (branch multi-fighter; PC build only). The two sides' leaders are fighters
 * 0 and 1 as ever; with every member on the field (gPortMultiAll, port/src/multi_fighter.c) each other member is a
 * fighter of its own, 2 and up, with a battle side record of its own (battle_load.c: BattleSide_GetCount,
 * BattleSide_GetTeam). Fighter i is also battle object i: the fighters' objects are created first, in order
 * (Battle_Load). On the console both are two and the opponent is the other fighter.
 */
#ifdef PORT
#define BTL_FIGHTER_MAX 10 /* 2 * BTL_MEMBER_MAX: a full team on each side */
/* The opponent of fighter i: the other one with two fighters (i ^ 1, as on the console), else the enemy it is set
   on (port/src/multi_fighter.c). */
extern s32 PortMulti_TargetOf(s32 fighter);
#define BTL_OPP(i) PortMulti_TargetOf(i)
/* The room the battle object work block keeps for the shadow pool's shadows (BtlObjPool3Work, battle/btl_obj.h):
   twelve (OBJ_SHADOW_MAX, sys/gfxm_e.h), one per battle object. btl_obj.c takes shadows from the free list after
   this room and gfxm_e_b.c fills the one of ObjShadowPool, padded to the same place (it checks that). */
#define BTL_SHADOW_ROOM (12 * 0x2100)
#else
#define BTL_FIGHTER_MAX 2
#define BTL_OPP(i) ((i) ^ 1)
#endif

#endif
