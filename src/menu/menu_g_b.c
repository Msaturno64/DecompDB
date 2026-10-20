#include "common.h"
#include "menu/menu_g.h"
#ifdef PORT
extern int Port_NetSession(void); /* port/src/gs/net.c */
extern s32 PortTeams_Run(void);   /* menu/port_teams.c: the team battle settings; 1 = on to the team select */
extern s32 PortNsel_Run(void);    /* menu/port_nsel.c: the select of an online match of more than two */
extern int Port_NetFighters(void); /* port/src/gs/net.c: the players' fighters and the computer's */
/* The PC's Team Battle screen, between the duel menu's Team and the team select (no game mode of its own: it runs
   here, where the versus archive it takes its windows from is loaded). Offline: an online session has no duel menu. */
#define DUEL_MODE_PORT_TEAMS 81
#define DUEL_PORT_TEAMS() (DUEL_PROG->battleType == 1 && !Port_NetSession())
#endif

/*
 * Duel_Main, 0x352CB8..0x352EC0: the handler of progress modes 38..41 (main-menu item 3, the duel mode).
 * Mode 38 is the duel menu, 39 and 40 are the two character selects that follow it (CharSel_Run and
 * TeamSel_Run, in earlier chunks; they write the battle setup), 41 is an empty screen. Returns non-zero
 * when the overlay has to return to the main executable (a battle was set up), zero to go back to the main
 * menu.
 */

extern s32 DuelMenu_Run(s32 section); /* 0x356090 (next chunk): the duel menu's frame loop */
extern s32 CharSel_Run(s32 section);  /* 0x348710: one-on-one character select; writes the battle setup */
extern s32 TeamSel_Run(s32 section);  /* 0x351508: team character select (team and DP battles); writes the battle setup */

s32 Duel_Mode41(s32 arg);

/* Runs the duel mode's screens until one of them leaves the mode. */
s32 Duel_Main(void) {
    s32 bgm = 0;
    s32 done = 0;
    s32 result = 1;

    if (gMenuArc5 == NULL) {
        gMenuArc5 = File_LoadSync(gProgress->baseFile, NULL, 0);
    }
    do {
        switch (gProgress->mode) {
        case 38:
            if (!bgm) {
                bgm = 1;
                Bgm_Play(DUEL_BGM);
            }
            if (DuelMenu_Run(1)) {
#ifdef PORT
                if (DUEL_PORT_TEAMS()) {
                    gProgress->mode = DUEL_MODE_PORT_TEAMS; /* (the duel menu's music goes on) */
                    break;
                }
#endif
                Adx_StopAll();
                gProgress->mode = DUEL_PROG->battleType ? 40 : 39;
            } else {
                Adx_StopAll();
                bgm = 0;
                gProgress->mode = 4;
                result = 0;
                done = 1;
            }
            break;
        case 39:
            if (CharSel_Run(2)) {
                Adx_StopAll();
                bgm = 0;
                done = 1;
            } else {
                Adx_StopAll();
                bgm = 0;
                gProgress->mode = 38;
#ifdef PORT /* PC build, online session: there is no versus menu in it; going back leaves for the main menu, which
               ends the session (as cancelling the versus menu does) */
                if (Port_NetSession()) {
                    gProgress->mode = 4;
                    result = 0;
                    done = 1;
                }
#endif
            }
            break;
#ifdef PORT
        case DUEL_MODE_PORT_TEAMS:
            if (!bgm) { /* (back from the team select) */
                bgm = 1;
                Bgm_Play(DUEL_BGM);
            }
            if (PortTeams_Run()) {
                Adx_StopAll();
                bgm = 0;
                gProgress->mode = 40;
            } else {
                gProgress->mode = 38;
            }
            break;
#endif
        case 40:
#ifdef PORT
            if (Port_NetSession() && Port_NetFighters() > 2) { /* (each player picks their own fighter) */
                if (!bgm) {
                    bgm = 1;
                    Bgm_Play(DUEL_BGM);
                }
                if (PortNsel_Run()) {
                    Adx_StopAll();
                    bgm = 0;
                    done = 1;
                } else { /* the host left: the session ends, as backing out of the team select does */
                    Adx_StopAll();
                    bgm = 0;
                    gProgress->mode = 4;
                    result = 0;
                    done = 1;
                }
                break;
            }
#endif
            if (TeamSel_Run(2)) {
                Adx_StopAll();
                bgm = 0;
                done = 1;
            } else {
                Adx_StopAll();
                bgm = 0;
                gProgress->mode = 38;
#ifdef PORT
                if (DUEL_PORT_TEAMS()) {
                    gProgress->mode = DUEL_MODE_PORT_TEAMS;
                }
#endif
#ifdef PORT /* PC build, online session: there is no versus menu in it; going back leaves for the main menu, which
               ends the session (as cancelling the versus menu does) */
                if (Port_NetSession()) {
                    gProgress->mode = 4;
                    result = 0;
                    done = 1;
                }
#endif
            }
            break;
        case 41:
            if (Duel_Mode41(0)) {
                Adx_StopAll();
                bgm = 0;
                done = 1;
                gProgress->mode = 39;
            } else {
                Adx_StopAll();
                bgm = 0;
                gProgress->mode = 39;
            }
            break;
        }
        sceGsSyncPath(0, 0);
    } while (!done);
    if (gMenuArc5 != NULL) {
        Heap_Free(gMenuArc5);
        gMenuArc5 = NULL;
    }
    return result;
}

/* The screen of mode 41: nothing; reports "done". */
s32 Duel_Mode41(s32 arg) {
    return 1;
}
