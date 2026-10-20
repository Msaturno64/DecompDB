#include "common.h"
#include "menu/menu_e.h"
#include "menu/menu_d.h"
#include "sys/pad.h"
#include "sys/save.h"

#ifdef PORT
/*
 * PC only: the character select of an online match of more than two players (SPEC-netplay-n.md). It opens where
 * the team select would (Duel_Main, menu_g_b.c) when the session has more than two players, and runs the same on
 * every copy: all it reads are the players' pads, which the match keeps the same everywhere (gs/net.c), so its
 * state is the game's (it goes back with a rollback and is part of the checks).
 * Each player has a small copy of the game's own select: a reel of seven chips, the row of the game's character grid
 * (section 42 of the select's pack, built as CharSel_Init builds it) with their cursor on it.
 *   Each player, on their own pad: the d-pad moves on the grid (ChrGrid_Move*), L1 / R1 the form (transformation),
 *   square the costume, cross ready (the choice is then kept), circle not ready.
 *   The host also: L2 the teams (how many on team 1), R2 the stage; triangle leaves (the session ends).
 *   A player who left (their pad carries the NET_GONE mark, btl_input.c) counts as ready: the computer plays them.
 * When everyone is ready the battle starts a second later. Player k plays fighter k (BattleSetup_PortSpread):
 * fighters 0 and 1 lead teams 1 and 2, then come team 1's others, then team 2's; so team 1 is players 0 and
 * 2 .. n1, team 2 player 1 and the rest. Free for all keeps that order, every fighter on its own.
 * Words in English or Spanish (Port_MenuLang); the kit and look of menu/port_teams.c. Every copy draws the same
 * picture (what is drawn is in the game's memory, which the copies compare): nothing on it is this copy's own, and
 * the players' names are everyone's (the host sends them all). Only the screen reader speaks for this copy's player.
 */
#include "menu/tbo_art.h"
#include "menu/tbo_sizes.h"
#include "menu/port_screen.h"

extern void Job_Run(void);
extern void Gfx_BeginFrame(void);
extern void Gfx_EndFrame(s32 arg);
extern void Dma_Flush(void);
extern void Dma_ResetBuffers(void);
extern void File_Stub264D90(void);
extern void ColorFade_StartIn(s32 r, s32 g, s32 b, s32 frames);
extern void ColorFade_StartOut(s32 r, s32 g, s32 b, s32 frames);
extern void ColorFade_Update(void);
extern void ColorFade_Draw(void);
extern s32 ColorFade_IsInDone(void);
extern s32 ColorFade_IsOutDone(void);
extern s32 sceGsSyncPath(s32 mode, u16 timeout);
extern void MsgWin_SetText(void *text);
extern void IconWin_PortHideWords(void);
extern void MsgWin_PortDrawString(u16 *str);
extern void MsgWin_PortSameTop(void);
extern int Port_PageAspectMilli(void);
extern int Port_MenuLang(void);                /* port/src/gs/ui.cpp: 0 English, 1 Spanish */
extern int Port_NetPlayers(void);              /* port/src/gs/net.c */
extern int Port_NetMe(void);
extern int Port_NetFfa(void);
extern const char *Port_NetName(int player);
extern void PortA11y_SayChanged(const char *text, int interrupt); /* port/src/a11y_game.c */
extern const char *PortA11y_StageName(int id);
extern char *PortTbo_FighterName(s32 chara);   /* menu/port_tbo.c */
extern s32 MChr_PortBt4Valid(s32 id);          /* menu/menu_c_e.c: a grid id that is a character */
extern s32 ChrGrid_IsSelectable(MChrCell *cells, s32 index);
extern s32 ChrGrid_MoveRight(MChrCell *cells, s32 *col, s32 row);
extern s32 ChrGrid_MoveLeft(MChrCell *cells, s32 *col, s32 row);
extern void ChrGrid_MoveDown(MChrCell *cells, s32 *col, s32 *row, s32 rows);
extern void ChrGrid_MoveUp(MChrCell *cells, s32 *col, s32 *row, s32 rows);
extern s32 ChrGrid_FixCursor(MChrCell *cells, s32 *col, s32 *row, s32 rows);
extern void ChrGrid_Build(s32 *outCount, MChrCell *out, s32 *inCount, MChrCell *in, s32 *customCount, MChrCell *custom);
extern s32 ChrTbl_WrapCostume(s32 chara, s32 *costume);
extern s32 CpuLevel_FromSetting(u32 setting);
extern u32 Rand_Range(u32 n);
extern void Battle_ClearWork(void);
extern void BattleSetup_SetRule(s32 screenMode, s32 mode, s32 bgm, s32 timeLimit, s32 announcer, s32 stage, s32 unk10);
extern void BattleSetup_SetSide(s32 sideNo, s32 control, s32 pad, s32 memberCount, s32 unk1FC, s32 unk200, s32 lead, void *unk);
extern void BattleSetup_SetMember(s32 sideNo, s32 idx, s32 chara, s32 costume, s32 variant, s32 cpuLevel, f32 health, void *items);
extern void BattleSetup_Finish(void);

#define NS_MAX 10
#define NS_SECTION 2      /* the select's pack in the duel's archive (CharSel_Run(2), TeamSel_Run(2)) */
#define NS_STAGES 0x23    /* the disc's stages (CHARSEL_STAGE_RANDOM is the first id after them) */
#define NS_START_DELAY 60 /* blanks between everyone ready and the battle */
#define NS_REEL 7         /* chips on a reel: a row of the grid (MCHR_COLS) */
#define NS_PAD_RES(pack, n) ((MTexRes *)MPACK_AT(pack, n))

enum {
    NX_TITLE, NX_PLAYERS, NX_TEAMS, NX_FFA, NX_STAGE, NX_RANDOM, NX_READY, NX_CPU, NX_COSTUME, NX_PLAYER,
    NX_HELP, NX_HELP_HOST, NX_HELP_READY, NX_STARTS, NX_FIGHTER, NX_OK, NX_BACK, NX_COUNT
};
static const char *const kNsText[NX_COUNT][2] = {
    {"ONLINE BATTLE", "BATALLA ONLINE"},
    {"%d PLAYERS", "%d JUGADORES"},
    {"TEAMS %d VS %d", "EQUIPOS %d CONTRA %d"},
    {"FREE FOR ALL", "TODOS CONTRA TODOS"},
    {"STAGE", "ESCENARIO"},
    {"RANDOM", "AL AZAR"},
    {"READY", "LISTO"},
    {"COMPUTER", "CPU"},
    {"COSTUME %d", "TRAJE %d"},
    {"PLAYER %d", "JUGADOR %d"},
    {"Each player: the d-pad picks the fighter, L1 / R1 the transformation, square the costume, cross ready.",
     "Cada jugador: la cruceta elige el personaje, L1 / R1 la transformaci\303\263n, cuadrado el traje, cruz listo."},
    {" The host: L2 the teams, R2 the stage.", " El anfitri\303\263n: L2 los equipos, R2 el escenario."},
    {"Ready! The battle starts when everyone is. Circle: change your fighter.",
     "\302\241Listo! La batalla empieza cuando est\303\251n todos. C\303\255rculo: cambiar tu personaje."},
    {"THE BATTLE STARTS!", "\302\241EMPIEZA LA BATALLA!"},
    {"Fighter", "Personaje"},
    {"Ready", "Listo"},
    {"Back", "Volver"},
};
#define NX(id) ((char *)kNsText[id][Port_MenuLang() == 1])

typedef struct PortNselScreen {
    s32 players;
    s32 ffa;
    s32 team1;          /* how many on team 1 (1 .. players - 1) */
    s32 stage;          /* -1 random, else a stage id */
    s32 col[NS_MAX];    /* each player's cursor on the grid */
    s32 row[NS_MAX];
    s32 form[NS_MAX];   /* which of the cell's forms */
    s32 costume[NS_MAX];
    s32 ready[NS_MAX];
    s32 pick[NS_MAX];   /* the character each one plays (set as the screen is left: "random" drawn then) */
    s32 allReady;       /* blanks since everyone was ready */
    s32 frames;
    s32 leaving;
    s32 result;
    void *winRes;       /* the Duel menu's screen unpacked: its windows */
    void *selRes;       /* the select's pack unpacked: the grid and the chips */
    u32 *chips;         /* a chip per grid id (id + 1) */
    s32 cellCount;
    s32 rows;
    MChrCell grid[256];            /* (MCHR_CELL_MAX on BT3; BT4's grid is larger) */
    MChrCell custom[MCHR_CUSTOM_MAX];
} PortNselScreen;
static PortNselScreen sNs;

/* Player k's pad left the match: the host plays it with the NET_GONE mark (gs/net.c kGone). */
static s32 Ns_Gone(s32 k) {
    return PAD_AT(k).data[17] == 0x47;
}

/* Which team player k is on: 0 or 1. */
static s32 Ns_Team(s32 k) {
    return k == 0 || (k >= 2 && k <= sNs.team1) ? 0 : 1;
}

static MChrCell *Ns_Cell(s32 k) {
    return &sNs.grid[sNs.col[k] + sNs.row[k] * MCHR_COLS];
}

/* The character under player k's cursor (its form), or MCHR_RANDOM. */
static s32 Ns_Chara(s32 k) {
    MChrCell *c = Ns_Cell(k);

    if (c->id == MCHR_RANDOM || !MChr_PortBt4Valid(c->id)) {
        return c->id;
    }
    return c->formCount > 0 ? c->form[sNs.form[k] % c->formCount] : c->id;
}

static char *Ns_Name(s32 chara) {
    return chara == MCHR_RANDOM ? NX(NX_RANDOM) : PortTbo_FighterName(chara);
}

static void Ns_Leave(s32 result) {
    sNs.leaving = 1;
    sNs.result = result;
    ColorFade_StartOut(0, 0, 0, 0x14);
}

/* One player's buttons. */
static void Ns_Player(s32 k) {
    Pad *pad = &PAD_AT(k);
    u32 rep = pad->gameRepeat;
    u32 hit = pad->gamePressed;
    MChrCell *c;
    s32 col = sNs.col[k];
    s32 row = sNs.row[k];

    if (Ns_Gone(k)) {
        sNs.ready[k] = 1;
        return;
    }
    if (sNs.ready[k]) {
        if (hit & PADG_CIRCLE) {
            sNs.ready[k] = 0;
            Snd_PlaySe(1, 2);
        }
        return;
    }
    if (rep & (PADG_RIGHT | PADG_RIGHT2)) {
        ChrGrid_MoveRight(sNs.grid, &sNs.col[k], sNs.row[k]);
    } else if (rep & (PADG_LEFT | PADG_LEFT2)) {
        ChrGrid_MoveLeft(sNs.grid, &sNs.col[k], sNs.row[k]);
    } else if (rep & (PADG_DOWN | PADG_DOWN2)) {
        ChrGrid_MoveDown(sNs.grid, &sNs.col[k], &sNs.row[k], sNs.rows);
    } else if (rep & (PADG_UP | PADG_UP2)) {
        ChrGrid_MoveUp(sNs.grid, &sNs.col[k], &sNs.row[k], sNs.rows);
    }
    c = Ns_Cell(k);
    if (sNs.col[k] != col || sNs.row[k] != row) {
        sNs.form[k] = 0;
        sNs.costume[k] = 0;
        Snd_PlaySe(1, 0);
    } else if ((rep & (PADG_L1 | PADG_R1)) && c->formCount > 1) {
        sNs.form[k] = (sNs.form[k] + ((rep & PADG_R1) ? 1 : c->formCount - 1)) % c->formCount;
        sNs.costume[k] = 0;
        Snd_PlaySe(1, 0);
    } else if ((hit & PADG_SQUARE) && MChr_PortBt4Valid(c->id)) {
        s32 n = ChrTbl_WrapCostume(Ns_Chara(k), NULL);

        sNs.costume[k] = n > 0 ? (sNs.costume[k] + 1) % n : 0;
        Snd_PlaySe(1, 0);
    }
    if ((hit & PADG_CROSS) && (c->id == MCHR_RANDOM || MChr_PortBt4Valid(c->id))) {
        sNs.ready[k] = 1;
        Snd_PlaySe(1, 1);
    }
}

static void Ns_Update(s32 live) {
    Pad *host = &PAD_AT(0);
    s32 k;
    s32 all = 1;

    Ps_Prompt(3, NX(NX_FIGHTER));
    Ps_Prompt(0, NX(NX_OK));
    Ps_Prompt(2, NX(NX_BACK)); /* (the host's) */
    if (!live) {
        return;
    }
    if (!sNs.ready[0]) { /* the host's choices, until the host is ready */
        if ((host->gamePressed & PADG_L2) && !sNs.ffa) {
            sNs.team1 = sNs.team1 % (sNs.players - 1) + 1;
            Snd_PlaySe(1, 0);
        }
        if (host->gameRepeat & PADG_R2) {
            sNs.stage = sNs.stage + 1 >= NS_STAGES ? -1 : sNs.stage + 1;
            Snd_PlaySe(1, 0);
        }
        if (host->gamePressed & PADG_TRIANGLE) {
            Snd_PlaySe(1, 2);
            Ns_Leave(0);
            return;
        }
    }
    for (k = 0; k < sNs.players; k++) {
        Ns_Player(k);
        all = all && sNs.ready[k];
    }
    sNs.allReady = all ? sNs.allReady + 1 : 0;
    if (sNs.allReady >= NS_START_DELAY) {
        Ns_Leave(1);
    }
}

/* What everyone plays: "random" drawn as the game's select draws it (a character cell, one of its forms). */
static void Ns_Pick(void) {
    s32 k;

    for (k = 0; k < sNs.players; k++) {
        sNs.pick[k] = Ns_Chara(k);
        if (sNs.pick[k] == MCHR_RANDOM || !MChr_PortBt4Valid(sNs.pick[k])) {
            MChrCell *c;

            do {
                c = &sNs.grid[Rand_Range(sNs.cellCount)];
            } while (!MChr_PortBt4Valid(c->id));
            sNs.pick[k] = c->formCount > 0 ? c->form[Rand_Range(c->formCount)] : c->id;
            sNs.costume[k] = 0;
        }
    }
}

/* The battle as chosen (the game's team select does the same for two, menu_e_b.c TeamSel_Run). */
static void Ns_Setup(void) {
    static u8 noItems[0x10]; /* an empty set of Z items */
    s32 team1 = sNs.ffa ? (sNs.players + 1) / 2 : sNs.team1;
    s32 timeLimit;
    s32 stage = sNs.stage;
    s32 bgm;
    s32 cpuLevel = CpuLevel_FromSetting(gSaveData->rule[1]);
    s32 k;

    if (stage < 0) {
        stage = (s32)Rand_Range(NS_STAGES);
    }
    bgm = (s32)Rand_Range(9) + 8; /* (a random song, as the team select's "random") */
    switch (gSaveData->rule[0]) {
    case 0:
        timeLimit = 1;
        break;
    case 1:
        timeLimit = 2;
        break;
    case 2:
        timeLimit = 3;
        break;
    case 3:
        timeLimit = 4;
        break;
    case 4:
        timeLimit = 0;
        break;
    default:
        timeLimit = 5;
        break;
    }
    Battle_ClearWork();
    BattleSetup_SetRule(1, 0, bgm, timeLimit, gSaveData->rule[2], stage, gSaveData->rule[5] ^ 1);
    BattleSetup_SetSide(0, 0, 0, team1, 1, 1, 0, NULL);
    BattleSetup_SetSide(1, 0, 1, sNs.players - team1, 1, 1, 0, NULL);
    BattleSetup_SetMember(0, 0, sNs.pick[0], sNs.costume[0], 0, cpuLevel, 100.0f, noItems);
    BattleSetup_SetMember(1, 0, sNs.pick[1], sNs.costume[1], 0, cpuLevel, 100.0f, noItems);
    for (k = 2; k < sNs.players; k++) { /* (fighter k: team 1's members after its lead, then team 2's) */
        if (k <= team1) {
            BattleSetup_SetMember(0, k - 1, sNs.pick[k], sNs.costume[k], 0, cpuLevel, 100.0f, noItems);
        } else {
            BattleSetup_SetMember(1, k - team1, sNs.pick[k], sNs.costume[k], 0, cpuLevel, 100.0f, noItems);
        }
    }
    BattleSetup_Finish();
}

/* ---- the picture: a card per player, two columns, as tall as the players leave room for ---- */
#define NS_CARDS_TOP 86.0f
#define NS_CARDS_BOTTOM 322.0f /* (the message box's top edge is at 326) */
#define NS_CARD_W 236.0f

static f32 Ns_CardH(void) {
    f32 h = (NS_CARDS_BOTTOM - NS_CARDS_TOP) / (f32)((sNs.players + 1) / 2) - 4.0f;

    return h > 64.0f ? 64.0f : h;
}

static void Ns_Card(s32 k, f32 *x0, f32 *y0) {
    *x0 = (k & 1) ? 262.0f : 14.0f;
    *y0 = NS_CARDS_TOP + (Ns_CardH() + 4.0f) * (f32)(k / 2);
}

/* Player k's reel: the seven chips of their grid row, the one under the cursor lit and larger. */
static void Ns_Reel(s32 k, f32 x, f32 cy, f32 h) {
    u8 *one = NS_PAD_RES(sNs.chips, 1)->tex;
    f32 w = h * Ps_GameTexShape(one) * 0.857f;
    f32 step;
    s32 i;

    if (w * 1.1f * NS_REEL > 176.0f) { /* (seven at most this wide: the rest of the card is for the words) */
        w = 176.0f / (1.1f * NS_REEL);
        h = w / (Ps_GameTexShape(one) * 0.857f);
    }
    step = w * 1.1f;
    for (i = 0; i < NS_REEL; i++) {
        s32 id = sNs.grid[i + sNs.row[k] * MCHR_COLS].id;
        f32 cx = x + step * ((f32)i + 0.5f);

        if (id == MCHR_EMPTY || i == sNs.col[k]) {
            continue;
        }
        Ps_GameTex(NS_PAD_RES(sNs.chips, id + 1)->tex, cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f, 0x585858, 0x80);
    }
    {   /* the cursor's chip last, on top */
        s32 id = Ns_Cell(k)->id;
        f32 cx = x + step * ((f32)sNs.col[k] + 0.5f);
        f32 s = 0.62f;

        Ps_Pill(TBO_KIT_SEL, cx - w * 0.7f, cy - h * 0.7f, cx + w * 0.7f, cy + h * 0.7f, 0x80);
        Ps_GameTex(NS_PAD_RES(sNs.chips, id + 1)->tex, cx - w * s, cy - h * s, cx + w * s, cy + h * s, PS_PLAIN, 0x80);
    }
}

static void Ns_Backdrop(void) {
    f32 aspect = (f32)Port_PageAspectMilli() / 1000.0f;
    f32 pic = Ps_PicW(TBO_BG) / Ps_PicH(TBO_BG);
    f32 u = aspect < pic ? aspect / pic : 1.0f;
    f32 v = aspect < pic ? 1.0f : pic / aspect;

    Ps_SpriteScreen(TBO_BG, 0.0f, 0.0f, 512.0f, 448.0f, 0.5f - u * 0.5f, 0.5f - v * 0.5f, 0.5f + u * 0.5f, 0.5f + v * 0.5f, PS_PLAIN, 0x80);
}

static void Ns_Draw(void) {
    f32 h = Ns_CardH();
    s32 k;

    for (k = 0; k < sNs.players; k++) {
        f32 x;
        f32 y;
        u32 c = sNs.ffa ? Ps_Rgba(150, 150, 150, 255) : Ns_Team(k) == 0 ? Ps_Rgba(240, 130, 30, 255) : Ps_Rgba(50, 120, 240, 255);

        Ns_Card(k, &x, &y);
        Ps_Pill(sNs.ready[k] ? TBO_KIT_SEL : TBO_KIT_BAR, x, y, x + NS_CARD_W, y + h, 0x80);
        Ps_Rect(x + 16.0f, y + h * 0.18f, x + 20.0f, y + h * 0.82f, c, c); /* the team's colour, inside the round end */
        Ns_Reel(k, x + 26.0f, y + h * 0.64f, h * 0.5f);
    }
    MsgWin_PortDrawString(NULL);
}

static void Ns_DrawText(void) {
    char line[200];
    char what[80];
    f32 h = Ns_CardH();
    f32 big = h < 50.0f ? 0.38f : 0.46f;
    s32 k;

    Ps_Heading(44.0f, Ps_TextTop(32.0f, 1.3f), NX(NX_TITLE), 1.3f, 300.0f, 0x80);
    sprintf(line, NX(NX_PLAYERS), (int)sNs.players);
    Ps_Cat(line, "  -  ", 200);
    if (sNs.ffa) {
        Ps_Cat(line, NX(NX_FFA), 200);
    } else {
        sprintf(what, NX(NX_TEAMS), (int)sNs.team1, (int)(sNs.players - sNs.team1));
        Ps_Cat(line, what, 200);
    }
    Ps_Cat(line, "  -  ", 200);
    Ps_Cat(line, NX(NX_STAGE), 200);
    Ps_Cat(line, ": ", 200);
    Ps_Cat(line, sNs.stage < 0 ? NX(NX_RANDOM) : (char *)PortA11y_StageName(sNs.stage), 200);
    Ps_Text(20.0f, Ps_TextTop(72.0f, 0.5f), line, 0.5f, 470.0f, 2);
    for (k = 0; k < sNs.players; k++) {
        f32 x;
        f32 y;
        const char *name = Port_NetName(k);

        Ns_Card(k, &x, &y);
        if (Ns_Gone(k)) {
            sprintf(what, "%d  %s", (int)(k + 1), NX(NX_CPU));
        } else if (name[0] != 0) {
            sprintf(what, "%d  %s", (int)(k + 1), name);
        } else {
            sprintf(what, NX(NX_PLAYER), (int)(k + 1));
        }
        Ps_Text(x + 28.0f, Ps_TextTop(y + h * 0.24f, 0.4f), what, 0.4f, 84.0f, 0);
        Ps_Align(2);
        Ps_Text(x + 222.0f, Ps_TextTop(y + h * 0.24f, big), Ns_Name(Ns_Chara(k)), big, 120.0f, sNs.ready[k] ? 5 : 3);
        sprintf(what, NX(NX_COSTUME), (int)(sNs.costume[k] + 1));
        Ps_Text(x + 222.0f, Ps_TextTop(y + h * 0.64f, 0.36f), what, 0.36f, 40.0f, 0);
        Ps_Align(0);
    }
    if (sNs.allReady > 0) {
        Ps_MsgLines(NX(NX_STARTS), 40.0f, 343.0f, 380.0f, 3);
    } else {
        Ps_Copy(line, NX(NX_HELP), 200);
        Ps_Cat(line, NX(NX_HELP_HOST), 200);
        Ps_MsgLines(line, 40.0f, 343.0f, 380.0f, 3);
    }
}

/* The screen reader: this copy's own player, what changes of it, and the host's choices. */
static void Ns_Speak(void) {
    char now[400];
    char what[80];
    s32 me = Port_NetMe();

    if (me < 0 || me >= sNs.players) {
        return;
    }
    now[0] = 0;
    if (sNs.frames < 2) {
        Ps_Copy(now, NX(NX_TITLE), 400);
        Ps_Cat(now, ". ", 400);
    }
    Ps_Cat(now, Ns_Name(Ns_Chara(me)), 400);
    Ps_Cat(now, ", ", 400);
    sprintf(what, NX(NX_COSTUME), (int)(sNs.costume[me] + 1));
    Ps_Cat(now, what, 400);
    if (sNs.ready[me]) {
        Ps_Cat(now, ". ", 400);
        Ps_Cat(now, NX(NX_HELP_READY), 400);
    }
    Ps_Cat(now, ". ", 400);
    if (sNs.ffa) {
        Ps_Cat(now, NX(NX_FFA), 400);
    } else {
        sprintf(what, NX(NX_TEAMS), (int)sNs.team1, (int)(sNs.players - sNs.team1));
        Ps_Cat(now, what, 400);
    }
    Ps_Cat(now, ". ", 400);
    Ps_Cat(now, NX(NX_STAGE), 400);
    Ps_Cat(now, ": ", 400);
    Ps_Cat(now, sNs.stage < 0 ? NX(NX_RANDOM) : (char *)PortA11y_StageName(sNs.stage), 400);
    if (sNs.allReady > 0) {
        Ps_Cat(now, ". ", 400);
        Ps_Cat(now, NX(NX_STARTS), 400);
    }
    PortA11y_SayChanged(now, 1);
}

static void Ns_WinInit(void) {
    void *icons;
    s32 count;
    s32 custom = 0;
    s32 i;

    sNs.winRes = Sprite_Unpack(MPACK_AT(gMenuArc5, 1), NULL, NULL); /* (the Duel menu's own, as port_teams.c) */
    icons = MPACK_AT(sNs.winRes, 24);
    Res_RelocateOffsets(&icons, icons, icons);
    IconWin_Init(MPACK_AT(sNs.winRes, 23), icons);
    IconWin_PortHideWords();
    IconWin_Open();
    MsgWin_Init(MPACK_AT(sNs.winRes, 22), MPACK_AT(sNs.winRes, 21), 1, 0);
    MsgWin_SetText(NULL);
    MsgWin_PortSameTop();
    MsgWin_Open();
    /* the grid and its chips, as CharSel_Init has them (menu_c_e.c) */
    sNs.selRes = Sprite_Unpack(MPACK_AT(gMenuArc5, NS_SECTION), NULL, NULL);
    sNs.chips = (u32 *)MPACK_AT(sNs.selRes, 45);
    for (i = 0; i < PORT_BT4(MCHR_CELL_MAX, 250); i++) {
        MTexRes *res = NS_PAD_RES(sNs.chips, i + 1);

        Res_RelocateOffsets(&res, res, res);
    }
    count = ((s32 *)sNs.selRes)[((u32 *)sNs.selRes)[42] >> 2];
    ChrGrid_Build(&sNs.cellCount, sNs.grid, &count, (MChrCell *)(MPACK_AT(sNs.selRes, 42) + 0x10), &custom, sNs.custom);
    sNs.rows = (sNs.cellCount + MCHR_COLS - 1) / MCHR_COLS;
}

static void Ns_WinTerm(void) {
    MsgWin_Term();
    IconWin_Term();
    Heap_Free(sNs.winRes);
    sNs.winRes = NULL;
    Heap_Free(sNs.selRes);
    sNs.selRes = NULL;
    sNs.chips = NULL;
}

/* From the session's start (or the last battle) until everyone is ready: 1 the battle is set up, 0 the host left. */
s32 PortNsel_Run(void) {
    s32 k;
    s32 keep = sNs.players == Port_NetPlayers(); /* (a rematch: the same choices, nobody ready) */

    sNs.players = Port_NetPlayers();
    sNs.ffa = Port_NetFfa();
    Ns_WinInit();
    if (!keep) {
        sNs.team1 = (sNs.players + 1) / 2;
        sNs.stage = -1;
        for (k = 0; k < NS_MAX; k++) { /* (each on a cell of their own to begin with) */
            sNs.col[k] = k % MCHR_COLS;
            sNs.row[k] = k / MCHR_COLS * 2;
            sNs.form[k] = 0;
            sNs.costume[k] = 0;
        }
    }
    for (k = 0; k < NS_MAX; k++) {
        ChrGrid_FixCursor(sNs.grid, &sNs.col[k], &sNs.row[k], sNs.rows);
        sNs.ready[k] = 0;
    }
    sNs.allReady = 0;
    sNs.frames = 0;
    sNs.leaving = 0;
    sNs.result = 0;
    ColorFade_StartIn(0, 0, 0, 0x14);
    while (1) {
        Job_Run();
        Gfx_BeginFrame();
        Pad_Update();
        Snd_Update();
        ColorFade_Update();
        Ps_PromptsClear();
        Ns_Update(!sNs.leaving && ColorFade_IsInDone());
        Ns_Speak();
        Ps_PageSet();
        Ns_Backdrop();
        IconWin_Draw();
        Ns_Draw();
        Ns_DrawText();
        Ps_PromptsDraw();
        ColorFade_Draw();
        File_Stub264D90();
        Gfx_EndFrame(1);
        Dma_Flush();
        sNs.frames++;
        if (sNs.leaving && ColorFade_IsOutDone()) {
            break;
        }
    }
    sceGsSyncPath(0, 0);
    if (sNs.result) {
        Ns_Pick();
    }
    Ns_WinTerm();
    Dma_ResetBuffers();
    if (sNs.result) {
        Ns_Setup();
    }
    return sNs.result;
}
#endif
