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
/* Battle objects (BTL_OBJ_MAX, battle/btl_obj.h): ten fighters and more than twenty for what their techniques
   bring (blast models, partners, props): the console's twelve ran out in a 5v5 (EftObj_Create gave -1), and twenty
   in a 5v2 free for all, where one of Frieza's techniques alone fires more than thirteen shots with a model. An
   effect that gets none goes on without its model (eft_ad_b.c). Past fifteen, models share alpha row 0
   (BtlObjVis_AllocFreeBit). */
#define BTL_OBJ_PORT_MAX 32
/* The room the battle object work block keeps for the shadow pool's shadows (BtlObjPool3Work, battle/btl_obj.h): one
   per battle object (OBJ_SHADOW_MAX, sys/gfxm_e.h). btl_obj.c takes shadows from the free list after this room and
   gfxm_e_b.c fills the one of ObjShadowPool, padded to the same place (it checks that). */
#define BTL_SHADOW_ROOM (BTL_OBJ_PORT_MAX * 0x2100)
/* Effect textures uploaded in one frame (EftAeVram, battle/eft_ae.h): five times the console's 128, for ten
   fighters' effects (a 5v5 ran past 128 and the list overwrote its own count). */
#define BTL_EFT_VRAM_MAX 640
/* Time stopped for one fighter: a model change (the change queue, BtlChars_IsTimeStopped) stops the changing
   fighter's (and its rush or throw pair's) input, gauges, timers and moves, not the field's (as the Tag Team Mod's
   cinematic_policy; port/src/multi_fighter.c). With two fighters, everybody's, as on the console. */
extern s32 PortMulti_TimeStoppedFor(s32 fighter);
#define BTL_TIME_STOPPED_FOR(fighter) PortMulti_TimeStoppedFor(fighter)
#else
#define BTL_FIGHTER_MAX 2
#define BTL_OPP(i) ((i) ^ 1)
#define BTL_TIME_STOPPED_FOR(fighter) BtlChars_IsTimeStopped()
#endif

#endif
