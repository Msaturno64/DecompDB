#ifndef PORT_MULTI_FIGHTER_H
#define PORT_MULTI_FIGHTER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Several fighters on the field (port/src/multi_fighter.c): a health bar over a fighter no HUD panel shows. Where
   its head is on the picture (0..1 of the game's 512 x 448 frame), its health, and whether it fights on the side of
   the HUD's own panel. */
typedef struct PortMultiBar {
    float x, y;
    int hp, hpMax;
    int ally;
} PortMultiBar;

/* The render thread's copy of the last drawn frame's bars; returns how many (at most max). */
int PortMulti_ReadBars(PortMultiBar *out, int max);

/* Whether a beam assist helps the HUD's own side in the last drawn frame ("BEAM ASSIST X1.5"). */
int PortMulti_ReadAssist(void);

/* The fighter the player watches while its own is down and the fight goes on (L3: the next one), -1 otherwise; its
   character (form) in *chara when chara is not NULL. */
int PortMulti_ReadWatch(int *chara);

#ifdef __cplusplus
}
#endif

#endif
