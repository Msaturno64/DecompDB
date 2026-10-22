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
 * Each player has a small copy of the game's own select: its reel (the blue bar, the frame of seven windows and the
 * cursor's frame, textures of the select's pack) and the steps of the game's select, each on their own pad:
 *   the team    (not in a free for all) left / right: team 1 or 2 (one full of players grows by a place, the other
 *               keeping one at least); cross
 *   the grid    the d-pad moves on the game's character grid (section 42, built as CharSel_Init builds it), the reel
 *               showing the cursor's row; cross takes the character (the "random" cell: done, drawn at the end)
 *   the forms   the reel shows the character's forms (transformations); left / right, cross takes one
 *   the costume left / right, cross: ready
 * Triangle goes a step back (the host's on the first step leaves: the session ends). The host also chooses the teams
 * (L2: how many on team 1, as many as the players on each side at least) and the computer's level (R2) until it is
 * ready. The computer's places fill what the players leave of each team. A player who left (their pad carries the
 * NET_GONE mark, btl_input.c) counts as ready: the computer plays them. So do the places nobody took (a room the host
 * started before it was full, gs/net.c Port_LobbyStartNow): ready from the start, a random fighter each.
 * When everyone is ready, a second later, the second screen: the stage and music vote (the user, 2026-10-22). Each
 * player moves a cursor of their own on the select's stage grid (its pictures, section 46), cross, then left / right
 * through its music list, cross (triangle: back, from the stage back to the fighters). When every player has voted:
 * one of the votes is drawn (all the same: that one), shown two seconds, and the battle starts.
 * Fighters 0 and 1 lead teams 1 and 2, then come team 1's
 * others, then team 2's (BattleSetup_PortSpread): Ns_Setup puts each team's places there in their order, and the
 * place (player, pad) of each fighter is PortNsel_PlaceOf. Free for all keeps the places' order (place k fighter k).
 * Words in English or Spanish (Port_MenuLang); the kit and look of menu/port_teams.c. Every copy draws the same
 * picture (what is drawn is in the game's memory, which the copies compare): nothing on it is this copy's own, and
 * the players' names are everyone's (the host sends them all). Only the screen reader speaks for this copy's player.
 * shortcut: BT3's reel only (its frame's windows measured on BT3's texture); BT4's hexagons when BT4 online has
 * more than two.
 * shortcut: the stage and song names are BT3's (port/src/a11y_game.c); BT4 shows its songs by number, and its
 * stages' names are BT3's list until BT4's are read from its disc.
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
extern int Port_NetFighters(void);
extern int Port_NetMe(void);
extern int Port_NetFfa(void);
extern const char *Port_NetName(int player);
extern void PortA11y_SayChanged(const char *text, int interrupt); /* port/src/a11y_game.c */
extern const char *PortA11y_StageName(int id);
extern const char *PortA11y_SongName(int v);
extern int PortA11y_TextLine(char *out, int cap, u32 file, int n); /* a line of a game text file, as UTF-8 */
extern char *PortTbo_FighterName(s32 chara);   /* menu/port_tbo.c */
extern s32 MChr_PortBt4Valid(s32 id);          /* menu/menu_c_e.c: a grid id that is a character */
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
#define NS_START_DELAY 60 /* blanks between everyone ready and the stage and music vote */
#define NS_RESULT_TIME 120 /* blanks the vote's result is shown before the battle */
#define NS_WORD(pack, n) (((s32 *)(pack))[((u32 *)(pack))[n] >> 2]) /* (menu_c_e.c MPACK_WORD) */
#define NS_REEL 7         /* chips on a reel: a row of the grid (MCHR_COLS), or a character's forms */
#define NS_RES(pack, n) ((MTexRes *)MPACK_AT(pack, n))
/* The reel's pictures (CharSel_Init's side textures): section 17 the bar (0) and the frame of seven windows (1),
   512 x 64; section 23 entry 0 the cursor's frame, 64 x 64. The frame's windows: 46 x 37 texels, the first at
   (83, 12), one every 60 (measured on BT3's). The reel is drawn from texel 72 on (the emblem before is left out).
   BT4's (PRIVATE, its dumped textures): entry 0 is the frame, with hexagonal windows 38 wide at 87, entry 1 the
   hexagons under the chips; its chips and cursor are 64 x 64 pictures centred on a window. */
#define NS_TEX_BAR MTEX(NS_RES(sNs.selRes, 17), PORT_BT4(0, 1))
#define NS_TEX_FRAME MTEX(NS_RES(sNs.selRes, 17), PORT_BT4(1, 0))
#define NS_TEX_CURSOR MTEX(NS_RES(sNs.selRes, 23), 0)
#define NS_REEL_S0 72.0f
#define NS_REEL_S1 512.0f
#define NS_WIN_S PORT_BT4(83.0f, 87.0f)
#define NS_WIN_STEP 60.0f
#define NS_WIN_W PORT_BT4(46.0f, 38.0f)
#define NS_WIN_T0 12.0f
#define NS_WIN_T1 49.0f

/* Each player's step (as the game's select has them), and of the vote. */
enum { NS_SIDE, NS_GRID, NS_FORM, NS_COSTUME, NS_READY };
enum { NS_V_STAGE, NS_V_MUSIC, NS_V_DONE };

enum {
    NX_TITLE, NX_PLAYERS, NX_TEAMS, NX_FFA, NX_STAGE, NX_RANDOM, NX_READY, NX_CPU, NX_COSTUME, NX_PLAYER,
    NX_HELP, NX_HELP_HOST, NX_HELP_READY, NX_STARTS, NX_MOVE, NX_OK, NX_BACK, NX_SIDE, NX_HELP_FFA, NX_CPU_LEVEL,
    NX_TO_VOTE, NX_VOTE_TITLE, NX_VOTE_RULE, NX_VOTE_HELP, NX_MUSIC, NX_TRACK, NX_RESULT, NX_LEVEL0, NX_COUNT = NX_LEVEL0 + 5
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
    {"COSTUME %d/%d", "TRAJE %d/%d"},
    {"PLAYER %d", "JUGADOR %d"},
    {"Each player: left / right the team, cross; then the d-pad and cross: the fighter, the transformation, the costume; triangle goes back.",
     "Cada jugador: izquierda / derecha el equipo, cruz; despu\303\251s la cruceta y cruz: el personaje, la transformaci\303\263n y el traje; tri\303\241ngulo vuelve."},
    {" The host: L2 the teams, R2 the computer's level.", " El anfitri\303\263n: L2 los equipos, R2 el nivel de la CPU."},
    {"Ready! The battle starts when everyone is. Triangle: change your choice.",
     "\302\241Listo! La batalla empieza cuando est\303\251n todos. Tri\303\241ngulo: cambiar tu elecci\303\263n."},
    {"THE BATTLE STARTS!", "\302\241EMPIEZA LA BATALLA!"},
    {"Move", "Mover"},
    {"Choose", "Elegir"},
    {"Back", "Volver"},
    {"<  TEAM %d  >", "<  EQUIPO %d  >"},
    {"Each player: the d-pad moves, cross chooses the fighter, then the transformation, then the costume; triangle goes back.",
     "Cada jugador: la cruceta mueve, cruz elige el personaje, despu\303\251s la transformaci\303\263n y el traje; tri\303\241ngulo vuelve."},
    {"COMPUTER: %s", "CPU: %s"},
    {"Everyone is ready: now the stage and the music.", "Todos listos: ahora el escenario y la m\303\272sica."},
    {"STAGE AND MUSIC", "ESCENARIO Y M\303\232SICA"},
    {"The same for everyone: that one. If not, one of the votes is drawn.",
     "Si todos eligen lo mismo, va eso; si no, se sortea entre los votos."},
    {"Each player: the d-pad and cross choose the stage, left / right and cross the music; triangle goes back.",
     "Cada jugador: la cruceta y cruz eligen el escenario, izquierda / derecha y cruz la m\303\272sica; tri\303\241ngulo vuelve."},
    {"MUSIC", "M\303\232SICA"},
    {"TRACK %d", "TEMA %d"},
    {"STAGE: %s  -  MUSIC: %s", "ESCENARIO: %s  -  M\303\232SICA: %s"},
    {"VERY EASY", "MUY F\303\201CIL"},
    {"EASY", "F\303\201CIL"},
    {"NORMAL", "NORMAL"},
    {"HARD", "DIF\303\215CIL"},
    {"VERY HARD", "MUY DIF\303\215CIL"},
};
#define NX(id) ((char *)kNsText[id][Port_MenuLang() == 1])

typedef struct PortNselScreen {
    s32 players;        /* the fighters: the players', then the computer's */
    s32 humans;         /* the players (those first) */
    s32 ffa;
    s32 team1;          /* how many on team 1 (1 .. players - 1) */
    s32 stage;          /* the vote's stage id and music-list value */
    s32 bgm;
    s32 cpu;            /* the computer's level, a setting 0..4 (CpuLevel_FromSetting) */
    s32 phase;          /* 0 the fighters, 1 the stage and music vote, 2 its result */
    s32 vote[NS_MAX];   /* NS_V_STAGE .. NS_V_DONE */
    s32 stg[NS_MAX];    /* each player's cursor on `stages`, and on `songs` */
    s32 song[NS_MAX];
    s32 stages[96];     /* the select's stages (every one: the copies' saves are not the same), "random" last */
    s32 stageCount;
    s32 songs[40];      /* its music list, "random" last */
    s32 songCount;
    s32 col[NS_MAX];    /* each player's cursor on the grid */
    s32 row[NS_MAX];
    s32 form[NS_MAX];   /* which of the cell's forms */
    s32 costume[NS_MAX];
    s32 step[NS_MAX];   /* NS_SIDE .. NS_READY */
    s32 team[NS_MAX];   /* each place's team, 0 or 1 */
    s32 place[NS_MAX];  /* each fighter's place, once Ns_Setup has set the battle up (mapped) */
    s32 mapped;
    s32 pick[NS_MAX];   /* the character each one plays (set as the screen is left: "random" drawn then) */
    s32 allReady;       /* blanks since everyone was ready */
    s32 frames;
    s32 running;        /* the select is on (for the online bots, PortNsel_BotButtons) */
    s32 leaving;
    s32 result;
    void *winRes;       /* the Duel menu's screen unpacked: its windows */
    void *selRes;       /* the select's pack unpacked: the grid, the chips, the reel's pictures */
    u32 *chips;         /* a chip per grid id (id + 1) */
    s32 cellCount;
    s32 rows;
    MChrCell grid[256]; /* (MCHR_CELL_MAX on BT3; BT4's grid is larger) */
    MChrCell custom[MCHR_CUSTOM_MAX];
} PortNselScreen;
static PortNselScreen sNs;

/* Player k's pad left the match: the host plays it with the NET_GONE mark (gs/net.c kGone). */
static s32 Ns_Gone(s32 k) {
    return k >= sNs.humans || PAD_AT(k).data[17] == 0x47; /* (or the place is the computer's from the start) */
}

/* Which team player k is on: 0 or 1. */
static s32 Ns_Team(s32 k) {
    return sNs.team[k];
}

/* The players (the places before the computer's) on team t. */
static s32 Ns_OnTeam(s32 t) {
    s32 n = 0;
    s32 k;

    for (k = 0; k < sNs.humans; k++) {
        n += sNs.team[k] == t;
    }
    return n;
}

static s32 Ns_TeamSize(s32 t) {
    return t == 0 ? sNs.team1 : sNs.players - sNs.team1;
}

/* The computer's places fill, in their order, what the players leave of team 1 and then of team 2. */
static void Ns_Fill(void) {
    s32 n0 = Ns_OnTeam(0);
    s32 k;

    for (k = sNs.humans; k < sNs.players; k++) {
        sNs.team[k] = n0 < sNs.team1 ? 0 : 1;
        n0 += sNs.team[k] == 0;
    }
}

/* The first step: the team's in a team battle. */
static s32 Ns_First(void) {
    return sNs.ffa ? NS_GRID : NS_SIDE;
}

static MChrCell *Ns_Cell(s32 k) {
    return &sNs.grid[sNs.col[k] + sNs.row[k] * MCHR_COLS];
}

static s32 Ns_Forms(s32 k) {
    s32 n = Ns_Cell(k)->formCount;

    return n < 1 ? 1 : n > NS_REEL ? NS_REEL : n;
}

/* The character under player k's cursor (its form), or a marker id (MCHR_RANDOM). */
static s32 Ns_Chara(s32 k) {
    MChrCell *c = Ns_Cell(k);

    if (!MChr_PortBt4Valid(c->id)) {
        return c->id;
    }
    return c->formCount > 0 ? c->form[sNs.form[k] % c->formCount] : c->id;
}

static s32 Ns_Costumes(s32 k) {
    s32 n = MChr_PortBt4Valid(Ns_Chara(k)) ? ChrTbl_WrapCostume(Ns_Chara(k), NULL) : 1;

    return n < 1 ? 1 : n;
}

/* (not `?:` between a table's entry and another pointer: the 32-bit pointer build read the entry as 8 bytes, a
   card of the computer's on the "random" cell crashed; as port_tbo.c Tb_Fighter notes) */
static char *Ns_Name(s32 chara) {
    if (chara == MCHR_RANDOM) {
        return NX(NX_RANDOM);
    }
    return PortTbo_FighterName(chara);
}

static char *Ns_StageName(s32 id) {
    if (id == CHARSEL_STAGE_RANDOM) {
        return NX(NX_RANDOM);
    }
    return (char *)PortA11y_StageName(id);
}

/* A song's name by music-list value (BT4's: its number in the list, into `buf`). */
static char *Ns_SongName(s32 v, char *buf) {
    s32 i;

    if (v == CHARSEL_BGM_RANDOM) {
        return NX(NX_RANDOM);
    }
    if (!gPortBt4) {
        return (char *)PortA11y_SongName(v);
    }
    for (i = 0; i < sNs.songCount && sNs.songs[i] != v; i++) {
    }
    sprintf(buf, NX(NX_TRACK), (int)(i + 1));
    return buf;
}

/* The stage grid's columns: six, as the game's, or more for a longer list (BT4's), in six rows. */
static s32 Ns_StageCols(void) {
    return sNs.stageCount > 36 ? (sNs.stageCount + 5) / 6 : 6;
}

static void Ns_Leave(s32 result) {
    sNs.leaving = 1;
    sNs.result = result;
    ColorFade_StartOut(0, 0, 0, 0x14);
}

/* Cross on a step: the next one, skipping a choice of one (a character with one form, a costume of one). */
static void Ns_Forward(s32 k) {
    s32 s = sNs.step[k];

    if (s == NS_GRID && !MChr_PortBt4Valid(Ns_Cell(k)->id)) {
        s = NS_READY; /* ("random": drawn when the battle is set up) */
    } else {
        do {
            s++;
        } while ((s == NS_FORM && Ns_Forms(k) < 2) || (s == NS_COSTUME && Ns_Costumes(k) < 2));
    }
    sNs.step[k] = s;
}

/* Triangle: the step before, as Ns_Forward skips them. */
static void Ns_Back(s32 k) {
    s32 s = sNs.step[k];

    if (s == NS_READY && !MChr_PortBt4Valid(Ns_Cell(k)->id)) {
        s = NS_GRID;
    } else {
        do {
            s--;
        } while ((s == NS_FORM && Ns_Forms(k) < 2) || (s == NS_COSTUME && Ns_Costumes(k) < 2));
    }
    sNs.step[k] = s;
}

/* One player's buttons. */
static void Ns_Player(s32 k) {
    Pad *pad = &PAD_AT(k);
    u32 rep = pad->gameRepeat;
    u32 hit = pad->gamePressed;
    s32 side = (rep & (PADG_RIGHT | PADG_RIGHT2)) ? 1 : (rep & (PADG_LEFT | PADG_LEFT2)) ? -1 : 0;

    if (Ns_Gone(k)) {
        sNs.step[k] = NS_READY;
        return;
    }
    if (hit & PADG_TRIANGLE) {
        if (sNs.step[k] == Ns_First()) {
            if (k == 0) { /* the host leaves the select: the session ends */
                Snd_PlaySe(1, 2);
                Ns_Leave(0);
            }
            return;
        }
        Ns_Back(k);
        Snd_PlaySe(1, 2);
        return;
    }
    if (hit & PADG_CROSS) {
        if (sNs.step[k] != NS_READY) {
            Ns_Forward(k);
            Snd_PlaySe(1, 1);
        }
        return;
    }
    switch (sNs.step[k]) {
    case NS_SIDE: {
        s32 t = side > 0 ? 1 : 0;

        if (side == 0 || sNs.team[k] == t) {
            break;
        }
        if (Ns_OnTeam(t) >= Ns_TeamSize(t)) { /* a team full of players: a place more for it, one less for the other */
            if (Ns_TeamSize(t ^ 1) < 2) {
                break;
            }
            sNs.team1 += t == 0 ? 1 : -1;
        }
        sNs.team[k] = t;
        Snd_PlaySe(1, 0);
        break;
    }
    case NS_GRID: {
        s32 col = sNs.col[k];
        s32 row = sNs.row[k];

        if (side > 0) {
            ChrGrid_MoveRight(sNs.grid, &sNs.col[k], sNs.row[k]);
        } else if (side < 0) {
            ChrGrid_MoveLeft(sNs.grid, &sNs.col[k], sNs.row[k]);
        } else if (rep & (PADG_DOWN | PADG_DOWN2)) {
            ChrGrid_MoveDown(sNs.grid, &sNs.col[k], &sNs.row[k], sNs.rows);
        } else if (rep & (PADG_UP | PADG_UP2)) {
            ChrGrid_MoveUp(sNs.grid, &sNs.col[k], &sNs.row[k], sNs.rows);
        }
        if (sNs.col[k] != col || sNs.row[k] != row) {
            sNs.form[k] = 0;
            sNs.costume[k] = 0;
            Snd_PlaySe(1, 0);
        }
        break;
    }
    case NS_FORM:
        if (side != 0) {
            sNs.form[k] = (sNs.form[k] + side + Ns_Forms(k)) % Ns_Forms(k);
            sNs.costume[k] = 0;
            Snd_PlaySe(1, 0);
        }
        break;
    case NS_COSTUME:
        if (side != 0) {
            sNs.costume[k] = (sNs.costume[k] + side + Ns_Costumes(k)) % Ns_Costumes(k);
            Snd_PlaySe(1, 0);
        }
        break;
    }
}

/* Everyone has their fighter: the stage and music vote of each player still here (the others vote nothing). */
static void Ns_VoteStart(void) {
    s32 k;

    sNs.phase = 1;
    sNs.allReady = 0;
    for (k = 0; k < NS_MAX; k++) {
        sNs.vote[k] = k < sNs.humans && !Ns_Gone(k) ? NS_V_STAGE : NS_V_DONE;
    }
    Snd_PlaySe(1, 1);
}

/* One player's buttons in the vote: the stage on the grid (the list in rows of Ns_StageCols), then the music. */
static void Ns_Voter(s32 k) {
    Pad *pad = &PAD_AT(k);
    u32 rep = pad->gameRepeat;
    u32 hit = pad->gamePressed;
    s32 side = (rep & (PADG_RIGHT | PADG_RIGHT2)) ? 1 : (rep & (PADG_LEFT | PADG_LEFT2)) ? -1 : 0;
    s32 n = sNs.stageCount;
    s32 cols = Ns_StageCols();
    s32 s = sNs.stg[k];

    if (Ns_Gone(k)) {
        sNs.vote[k] = NS_V_DONE;
        return;
    }
    if (hit & PADG_TRIANGLE) {
        if (sNs.vote[k] == NS_V_STAGE) { /* back to the fighters, this player's last choice open again */
            sNs.phase = 0;
            sNs.allReady = 0;
            Ns_Back(k);
        } else {
            sNs.vote[k]--;
        }
        Snd_PlaySe(1, 2);
        return;
    }
    if (hit & PADG_CROSS) {
        if (sNs.vote[k] != NS_V_DONE) {
            sNs.vote[k]++;
            Snd_PlaySe(1, 1);
        }
        return;
    }
    if (sNs.vote[k] == NS_V_STAGE) {
        if (side != 0) {
            s = (s + side + n) % n;
        } else if (rep & (PADG_DOWN | PADG_DOWN2)) {
            s = s + cols < n ? s + cols : s % cols;
        } else if (rep & (PADG_UP | PADG_UP2)) {
            s = s >= cols ? s - cols : s + (n - 1 - s) / cols * cols;
        }
        if (s != sNs.stg[k]) {
            sNs.stg[k] = s;
            Snd_PlaySe(1, 0);
        }
    } else if (sNs.vote[k] == NS_V_MUSIC && side != 0) {
        sNs.song[k] = (sNs.song[k] + side + sNs.songCount) % sNs.songCount;
        Snd_PlaySe(1, 0);
    }
}

/* Every vote is in: one of them is drawn (each vote a ticket; all the same, that one), "random" drawn among the list. */
static void Ns_VoteResult(void) {
    s32 n = 0;
    s32 k;
    s32 pick;

    sNs.stage = CHARSEL_STAGE_RANDOM;
    sNs.bgm = CHARSEL_BGM_RANDOM;
    for (k = 0; k < sNs.humans; k++) {
        n += !Ns_Gone(k);
    }
    if (n > 0) {
        pick = (s32)Rand_Range(n);
        for (k = 0; k < sNs.humans; k++) {
            if (!Ns_Gone(k) && pick-- == 0) {
                sNs.stage = sNs.stages[sNs.stg[k]];
            }
        }
        pick = (s32)Rand_Range(n);
        for (k = 0; k < sNs.humans; k++) {
            if (!Ns_Gone(k) && pick-- == 0) {
                sNs.bgm = sNs.songs[sNs.song[k]];
            }
        }
    }
    if (sNs.stage == CHARSEL_STAGE_RANDOM) {
        sNs.stage = sNs.stages[Rand_Range(sNs.stageCount - 1)];
    }
    if (sNs.bgm == CHARSEL_BGM_RANDOM) {
        sNs.bgm = sNs.songs[Rand_Range(sNs.songCount - 1)];
    }
    sNs.phase = 2;
    sNs.allReady = 0;
    Snd_PlaySe(1, 1);
}

static void Ns_Update(s32 live) {
    Pad *host = &PAD_AT(0);
    s32 k;
    s32 all = 1;

    Ps_Prompt(3, NX(NX_MOVE));
    Ps_Prompt(0, NX(NX_OK));
    Ps_Prompt(2, NX(NX_BACK));
    if (!live) {
        return;
    }
    if (sNs.phase == 2) {
        if (++sNs.allReady >= NS_RESULT_TIME) {
            Ns_Leave(1);
        }
        return;
    }
    if (sNs.phase == 1) {
        for (k = 0; k < sNs.humans && sNs.phase == 1; k++) {
            Ns_Voter(k);
        }
        for (k = 0; k < NS_MAX && sNs.phase == 1; k++) {
            all = all && sNs.vote[k] == NS_V_DONE;
        }
        if (sNs.phase == 1 && all) {
            Ns_VoteResult();
        }
        return;
    }
    if (sNs.step[0] != NS_READY) { /* the host's choices, until the host is ready */
        if ((host->gamePressed & PADG_L2) && !sNs.ffa) {
            do { /* (the next split that holds the players on each side; the one it had does) */
                sNs.team1 = sNs.team1 % (sNs.players - 1) + 1;
            } while (Ns_OnTeam(0) > Ns_TeamSize(0) || Ns_OnTeam(1) > Ns_TeamSize(1));
            Snd_PlaySe(1, 0);
        }
        if (host->gamePressed & PADG_R2) {
            sNs.cpu = (sNs.cpu + 1) % 5;
            Snd_PlaySe(1, 0);
        }
    }
    for (k = 0; k < sNs.players && !sNs.leaving; k++) {
        if (k < sNs.humans) {
            Ns_Player(k);
        }
        all = all && sNs.step[k] == NS_READY;
    }
    Ns_Fill();
    sNs.allReady = all && !sNs.leaving ? sNs.allReady + 1 : 0;
    if (sNs.allReady >= NS_START_DELAY) {
        Ns_VoteStart();
    }
}

/* What everyone plays: "random" drawn as the game's select draws it (a character cell, one of its forms). */
static void Ns_Pick(void) {
    s32 k;

    for (k = 0; k < sNs.players; k++) {
        sNs.pick[k] = Ns_Chara(k);
        if (!MChr_PortBt4Valid(sNs.pick[k])) {
            MChrCell *c;

            do {
                c = &sNs.grid[Rand_Range(sNs.cellCount)];
            } while (!MChr_PortBt4Valid(c->id));
            sNs.pick[k] = c->formCount > 0 ? c->form[Rand_Range(c->formCount)] : c->id;
            sNs.costume[k] = 0;
        }
    }
}

/* Each fighter's place: team 1's lead, team 2's, team 1's others, team 2's, each team's places in their order. */
static void Ns_Order(s32 team1) {
    s32 n[2] = {0, 0};
    s32 k;

    for (k = 0; k < sNs.players; k++) {
        s32 t = sNs.team[k];

        sNs.place[n[t] == 0 ? t : t == 0 ? n[0] + 1 : team1 + n[1]] = k;
        n[t]++;
    }
    sNs.mapped = 1;
}

/* The place (player, pad) that plays fighter f; f itself without this select (a battle of two). */
s32 PortNsel_PlaceOf(s32 f) {
    return sNs.mapped && f >= 0 && f < sNs.players ? sNs.place[f] : f;
}

/* The fighter of place p. */
s32 PortNsel_FighterOf(s32 p) {
    s32 f;

    for (f = 0; sNs.mapped && f < sNs.players; f++) {
        if (sNs.place[f] == p) {
            return f;
        }
    }
    return p;
}

/* The battle as chosen (the game's team select does the same for two, menu_e_b.c TeamSel_Run). */
static void Ns_Setup(void) {
    static u8 noItems[0x10]; /* an empty set of Z items */
    s32 team1 = sNs.ffa ? (sNs.players + 1) / 2 : sNs.team1;
    s32 timeLimit;
    s32 stage = sNs.stage; /* (the vote's: Ns_VoteResult) */
    s32 bgm = sNs.bgm;
    s32 cpuLevel = CpuLevel_FromSetting(sNs.cpu);
    s32 k;

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
    Ns_Order(team1);
    Battle_ClearWork();
    BattleSetup_SetRule(1, 0, bgm, timeLimit, gSaveData->rule[2], stage, gSaveData->rule[5] ^ 1);
    for (k = 0; k < 2; k++) { /* (control 0 a pad, 2 the computer: a lead on a computer's place, no player's pad) */
        s32 p = sNs.place[k];

        BattleSetup_SetSide(k, p < sNs.humans ? 0 : 2, p, k == 0 ? team1 : sNs.players - team1, 1, 1, 0, NULL);
        BattleSetup_SetMember(k, 0, sNs.pick[p], sNs.costume[p], 0, cpuLevel, 100.0f, noItems);
    }
    for (k = 2; k < sNs.players; k++) { /* (fighter k: team 1's members after its lead, then team 2's) */
        s32 p = sNs.place[k];

        if (k <= team1) {
            BattleSetup_SetMember(0, k - 1, sNs.pick[p], sNs.costume[p], 0, cpuLevel, 100.0f, noItems);
        } else {
            BattleSetup_SetMember(1, k - team1, sNs.pick[p], sNs.costume[p], 0, cpuLevel, 100.0f, noItems);
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

/* Player k's reel, `w` units wide from (x, y): the bar, the chips in the frame's windows (the grid row on the grid,
   the character's forms after it), the frame, and the cursor's frame in gold on the chosen chip. */
static void Ns_Reel(s32 k, f32 x, f32 y, f32 w) {
    f32 su = w / (NS_REEL_S1 - NS_REEL_S0);    /* units per texel across */
    f32 sv = su / 0.857f;                      /* ... and down (a unit across is 7:6 of one down) */
    s32 onGrid = sNs.step[k] <= NS_GRID; /* (on the team's step too: the grid's row it starts on) */
    s32 cur = onGrid ? sNs.col[k] : sNs.form[k];
    s32 i;

    Ps_GameTexPart(NS_TEX_BAR, x, y, x + w, y + 64.0f * sv, NS_REEL_S0, 0.0f, NS_REEL_S1, 64.0f, PS_PLAIN, 0x80);
    for (i = 0; i < NS_REEL; i++) {
        MChrCell *c = Ns_Cell(k);
        s32 id = onGrid ? sNs.grid[i + sNs.row[k] * MCHR_COLS].id : i < Ns_Forms(k) && c->formCount > 0 ? c->form[i] : i == 0 ? c->id : MCHR_EMPTY;
        f32 x0 = x + (NS_WIN_S + NS_WIN_STEP * (f32)i - NS_REEL_S0) * su;

        if (id != MCHR_EMPTY && gPortBt4) {
            f32 cx = x0 + NS_WIN_W * 0.5f * su;
            f32 cy = y + (NS_WIN_T0 + NS_WIN_T1) * 0.5f * sv;

            Ps_GameTex(NS_RES(sNs.chips, id + 1)->tex, cx - 32.0f * su, cy - 32.0f * sv, cx + 32.0f * su, cy + 32.0f * sv,
                       i == cur || sNs.step[k] == NS_GRID ? PS_PLAIN : 0x505050, 0x80);
        } else if (id != MCHR_EMPTY) {
            Ps_GameTex(NS_RES(sNs.chips, id + 1)->tex, x0, y + NS_WIN_T0 * sv, x0 + NS_WIN_W * su, y + NS_WIN_T1 * sv,
                       i == cur || sNs.step[k] == NS_GRID ? PS_PLAIN : 0x505050, 0x80);
        }
    }
    Ps_GameTexPart(NS_TEX_FRAME, x, y, x + w, y + 64.0f * sv, NS_REEL_S0, 0.0f, NS_REEL_S1, 64.0f, PS_PLAIN, 0x80);
    if (gPortBt4) { /* (its cursor is a hexagon of its own colour) */
        f32 cx = x + (NS_WIN_S + NS_WIN_STEP * (f32)cur + NS_WIN_W * 0.5f - NS_REEL_S0) * su;
        f32 cy = y + (NS_WIN_T0 + NS_WIN_T1) * 0.5f * sv;

        Ps_GameTex(NS_TEX_CURSOR, cx - 32.0f * su, cy - 32.0f * sv, cx + 32.0f * su, cy + 32.0f * sv,
                   sNs.step[k] == NS_READY ? 0x208060 : PS_PLAIN, 0x80);
    } else {
        f32 x0 = x + (NS_WIN_S + NS_WIN_STEP * (f32)cur - NS_REEL_S0 - 5.0f) * su;
        f32 x1 = x + (NS_WIN_S + NS_WIN_STEP * (f32)cur + NS_WIN_W - NS_REEL_S0 + 5.0f) * su;

        Ps_GameTex(NS_TEX_CURSOR, x0, y + (NS_WIN_T0 - 5.0f) * sv, x1, y + (NS_WIN_T1 + 5.0f) * sv,
                   sNs.step[k] == NS_READY ? 0x208060 : 0x106880, 0x80); /* gold; green when ready */
    }
}

static void Ns_Backdrop(void) {
    f32 aspect = (f32)Port_PageAspectMilli() / 1000.0f;
    f32 pic = Ps_PicW(TBO_BG) / Ps_PicH(TBO_BG);
    f32 u = aspect < pic ? aspect / pic : 1.0f;
    f32 v = aspect < pic ? 1.0f : pic / aspect;

    Ps_SpriteScreen(TBO_BG, 0.0f, 0.0f, 512.0f, 448.0f, 0.5f - u * 0.5f, 0.5f - v * 0.5f, 0.5f + u * 0.5f, 0.5f + v * 0.5f, PS_PLAIN, 0x80);
}

/* A card's inside: from its x, what lies between the bar's round ends (x0, and the width w). */
static void Ns_Inside(f32 x, f32 h, f32 *x0, f32 *w) {
    f32 cap = Ps_PillCap(TBO_KIT_BAR, h);

    *x0 = x + cap * 0.75f;
    *w = NS_CARD_W - cap * 1.5f;
}

/* The reel's width on a card of height h: what the card's inside leaves after the team's colour, as tall as 55 %
   of the card at most. */
static f32 Ns_ReelW(f32 h) {
    f32 x0;
    f32 in;
    f32 w = 0.55f * h * 0.857f * (NS_REEL_S1 - NS_REEL_S0) / 64.0f;

    Ns_Inside(0.0f, h, &x0, &in);
    return w > in - 14.0f ? in - 14.0f : w;
}

/* The reel's height for that width. */
static f32 Ns_ReelH(f32 w) {
    return 64.0f * w / (NS_REEL_S1 - NS_REEL_S0) / 0.857f;
}

static void Ns_Draw(void) {
    f32 h = Ns_CardH();
    f32 w = Ns_ReelW(h);
    s32 k;

    for (k = 0; k < sNs.players; k++) {
        f32 x;
        f32 y;
        u32 c = sNs.ffa ? Ps_Rgba(150, 150, 150, 255) : Ns_Team(k) == 0 ? Ps_Rgba(240, 130, 30, 255) : Ps_Rgba(50, 120, 240, 255);

        Ns_Card(k, &x, &y);
        Ps_Pill(sNs.step[k] == NS_READY ? TBO_KIT_SEL : TBO_KIT_BAR, x, y, x + NS_CARD_W, y + h, 0x80);
        {
            f32 x0;
            f32 in;

            Ns_Inside(x, h, &x0, &in);
            Ps_Rect(x0 + 5.0f, y + h * 0.3f, x0 + 9.0f, y + h * 0.84f, c, c); /* the team's colour, clear of the border */
            Ns_Reel(k, x0 + 14.0f + (in - 14.0f - w) * 0.5f, y + h * 0.62f - Ns_ReelH(w) * 0.5f, w);
        }
    }
    MsgWin_PortDrawString(NULL);
}

/* What a card says of player k's choice: the character (its form), and the costume once it is chosen. */
static void Ns_Choice(s32 k, char *out) {
    if (sNs.step[k] == NS_SIDE) {
        sprintf(out, NX(NX_SIDE), (int)(sNs.team[k] + 1));
        return;
    }
    Ps_Copy(out, Ns_Name(Ns_Chara(k)), 80);
    if (sNs.step[k] >= NS_COSTUME && Ns_Costumes(k) > 1) {
        char what[64];

        /* (BT4: the costume's name, as its select shows it: line `costume` of the character's list in section 57,
           menu_c_e.c MenuColor_PortBt4Name) */
        if (!gPortBt4 || PortA11y_TextLine(what, sizeof(what), (u32)MPACK_AT(MPACK_AT(sNs.selRes, 57), Ns_Chara(k) + 1),
                                           sNs.costume[k]) <= 0) {
            sprintf(what, NX(NX_COSTUME), (int)(sNs.costume[k] + 1), (int)Ns_Costumes(k));
        }
        Ps_Cat(out, "  ", 80);
        Ps_Cat(out, what, 80);
    }
}

static void Ns_DrawText(void) {
    char line[200];
    char what[80];
    f32 h = Ns_CardH();
    f32 big = h < 50.0f ? 0.38f : 0.44f;
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
    sprintf(what, NX(NX_CPU_LEVEL), NX(NX_LEVEL0 + sNs.cpu));
    Ps_Cat(line, what, 200);
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
        {
            f32 x0;
            f32 in;

            Ns_Inside(x, h, &x0, &in);
            x0 += 4.0f; /* (the round ends close in at the top: the words keep off them a little more) */
            in -= 8.0f;
            Ps_Text(x0, Ps_TextTop(y + h * 0.24f, 0.4f), what, 0.4f, 68.0f, 0);
            Ns_Choice(k, what);
            Ps_Align(2);
            Ps_Text(x0 + in, Ps_TextTop(y + h * 0.24f, big), what, big, in - 74.0f, sNs.step[k] == NS_READY ? 5 : 3);
            Ps_Align(0);
        }
    }
    if (sNs.allReady > 0) {
        Ps_MsgLines(NX(NX_TO_VOTE), 40.0f, 343.0f, 380.0f, 3);
    } else {
        Ps_Copy(line, NX(sNs.ffa ? NX_HELP_FFA : NX_HELP), 200);
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
    if (sNs.phase != 0) { /* the vote: this player's stage and music, then the result */
        char buf[24];

        Ps_Copy(now, NX(NX_VOTE_TITLE), 400);
        Ps_Cat(now, ". ", 400);
        if (sNs.phase == 2) {
            sprintf(what, NX(NX_RESULT), Ns_StageName(sNs.stage), Ns_SongName(sNs.bgm, buf));
            Ps_Cat(now, what, 400);
        } else if (me < sNs.humans) {
            Ps_Cat(now, Ns_StageName(sNs.stages[sNs.stg[me]]), 400);
            Ps_Cat(now, ". ", 400);
            Ps_Cat(now, NX(NX_MUSIC), 400);
            Ps_Cat(now, ": ", 400);
            Ps_Cat(now, Ns_SongName(sNs.songs[sNs.song[me]], buf), 400);
            if (sNs.vote[me] == NS_V_DONE) {
                Ps_Cat(now, ". ", 400);
                Ps_Cat(now, NX(NX_READY), 400);
            }
        }
        PortA11y_SayChanged(now, 1);
        return;
    }
    if (sNs.frames < 2) {
        Ps_Copy(now, NX(NX_TITLE), 400);
        Ps_Cat(now, ". ", 400);
    }
    Ns_Choice(me, what);
    Ps_Cat(now, what, 400);
    if (sNs.step[me] == NS_READY) {
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
    sprintf(what, NX(NX_CPU_LEVEL), NX(NX_LEVEL0 + sNs.cpu));
    Ps_Cat(now, what, 400);
    if (sNs.allReady > 0) {
        Ps_Cat(now, ". ", 400);
        Ps_Cat(now, NX(NX_TO_VOTE), 400);
    }
    PortA11y_SayChanged(now, 1);
}

static void Ns_WinInit(void) {
    static const s32 kReelRes[2] = {17, 23}; /* (the reel's pictures, see NS_TEX_*) */
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
    /* the grid, its chips and the reel's pictures, as CharSel_Init has them (menu_c_e.c) */
    sNs.selRes = Sprite_Unpack(MPACK_AT(gMenuArc5, NS_SECTION), NULL, NULL);
    for (i = 0; i < 2; i++) {
        MTexRes *res = NS_RES(sNs.selRes, kReelRes[i]);

        Res_RelocateOffsets(&res, res, res);
    }
    sNs.chips = (u32 *)MPACK_AT(sNs.selRes, 45);
    for (i = 0; i < PORT_BT4(MCHR_CELL_MAX, 250); i++) {
        MTexRes *res = NS_RES(sNs.chips, i + 1);

        Res_RelocateOffsets(&res, res, res);
    }
    count = ((s32 *)sNs.selRes)[((u32 *)sNs.selRes)[42] >> 2];
    ChrGrid_Build(&sNs.cellCount, sNs.grid, &count, (MChrCell *)(MPACK_AT(sNs.selRes, 42) + 0x10), &custom, sNs.custom);
    sNs.rows = (sNs.cellCount + MCHR_COLS - 1) / MCHR_COLS;
    /* the vote's stages and music: the select's lists as CharSel_Init reads them (sections 39 and 56, the stage
       pictures 46), without the save's locks; the stages its "random" draws from (BT4's past the markers too) */
    {
        s32 *ids = (s32 *)(MPACK_AT(sNs.selRes, 39) + 0x10);
        s32 n = NS_WORD(sNs.selRes, 39);

        for (i = 0; i < PORT_BT4(38, 99); i++) {
            MTexRes *res = NS_RES(MPACK_AT(sNs.selRes, 46), i + 1);

            Res_RelocateOffsets(&res, res, res);
        }
        sNs.stageCount = 0;
        for (i = 0; i < n && sNs.stageCount < 95; i++) {
            if (ids[i] >= 0 && (ids[i] < CHARSEL_STAGE_RANDOM || (gPortBt4 && ids[i] >= 39))) {
                sNs.stages[sNs.stageCount++] = ids[i];
            }
        }
        sNs.stages[sNs.stageCount++] = CHARSEL_STAGE_RANDOM;
        ids = (s32 *)(MPACK_AT(sNs.selRes, 56) + 0x10);
        n = NS_WORD(sNs.selRes, 56) - 4; /* (its last four are not songs; CharSel_Init makes the last "random") */
        sNs.songCount = 0;
        for (i = 0; i < n - 1 && sNs.songCount < 39; i++) {
            if (ids[i] >= 0 && ids[i] < CHARSEL_BGM_RANDOM) {
                sNs.songs[sNs.songCount++] = ids[i];
            }
        }
        sNs.songs[sNs.songCount++] = CHARSEL_BGM_RANDOM;
    }
}

/* A stage's picture (the select's icon pack: one per stage id, "random" too). */
static u8 *Ns_StageTex(s32 id) {
    return NS_RES(MPACK_AT(sNs.selRes, 46), id + 1)->tex;
}

/* ---- the vote's picture: the stage grid on the left with each player's cursor, the votes on the right ---- */
#define NS_GRID_X1 300.0f
#define NS_VOTE_X0 308.0f

/* Stage i's cell on the grid. */
static void Ns_StageCell(s32 i, f32 *x, f32 *y, f32 *w, f32 *h) {
    s32 cols = Ns_StageCols();

    *w = (NS_GRID_X1 - 14.0f) / (f32)cols;
    *h = (NS_CARDS_BOTTOM - NS_CARDS_TOP) / (f32)((sNs.stageCount + cols - 1) / cols);
    *x = 14.0f + *w * (f32)(i % cols);
    *y = NS_CARDS_TOP + *h * (f32)(i / cols);
}

/* How many players before k have their cursor on k's cell (their frames nest, their numbers go side by side). */
static s32 Ns_Before(s32 k) {
    s32 n = 0;
    s32 j;

    for (j = 0; j < k; j++) {
        n += !Ns_Gone(j) && sNs.stg[j] == sNs.stg[k];
    }
    return n;
}

static u32 Ns_Color(s32 k) {
    if (sNs.ffa) {
        return Ps_Rgba(200, 200, 200, 255);
    }
    return Ns_Team(k) == 0 ? Ps_Rgba(240, 130, 30, 255) : Ps_Rgba(50, 120, 240, 255);
}

static void Ns_Frame(f32 x0, f32 y0, f32 x1, f32 y1, f32 t, u32 c) {
    Ps_Rect(x0, y0, x1, y0 + t, c, c);
    Ps_Rect(x0, y1 - t, x1, y1, c, c);
    Ps_Rect(x0, y0, x0 + t, y1, c, c);
    Ps_Rect(x1 - t, y0, x1, y1, c, c);
}

/* A row of the votes' list: as tall as the players leave room for. */
static f32 Ns_VoteRowH(void) {
    f32 h = (NS_CARDS_BOTTOM - NS_CARDS_TOP) / (f32)sNs.humans - 3.0f;

    return h > 44.0f ? 44.0f : h;
}

static void Ns_DrawVote(void) {
    f32 x, y, cw, ch;
    s32 i, k;

    for (i = 0; i < sNs.stageCount; i++) {
        u8 *tex = Ns_StageTex(sNs.stages[i]);
        f32 r = Ps_GameTexShape(tex);
        f32 w, h;

        Ns_StageCell(i, &x, &y, &cw, &ch);
        w = cw - 4.0f;
        h = ch - 4.0f;
        if (w > h * r) { /* (the picture's own shape, centred in its cell) */
            w = h * r;
        } else {
            h = w / r;
        }
        Ps_GameTex(tex, x + (cw - w) * 0.5f, y + (ch - h) * 0.5f, x + (cw + w) * 0.5f, y + (ch + h) * 0.5f,
                   sNs.phase == 2 && sNs.stages[i] != sNs.stage ? 0x505050 : PS_PLAIN, 0x80);
        if (sNs.phase == 2 && sNs.stages[i] == sNs.stage) {
            Ns_Frame(x, y, x + cw, y + ch, 3.0f, Ps_Rgba(255, 210, 60, 255)); /* (the stage drawn: gold) */
        }
    }
    for (k = 0; k < sNs.humans && sNs.phase == 1; k++) {
        f32 in = 1.0f + 2.5f * (f32)Ns_Before(k);

        if (!Ns_Gone(k)) {
            Ns_StageCell(sNs.stg[k], &x, &y, &cw, &ch);
            Ns_Frame(x + in, y + in, x + cw - in, y + ch - in, 2.0f, Ns_Color(k));
        }
    }
    y = NS_CARDS_TOP - Ns_VoteRowH() - 3.0f;
    for (k = 0; k < sNs.humans; k++) { /* the votes: a bar each, lit once the player has voted */
        f32 rh = Ns_VoteRowH();

        y += rh + 3.0f; /* (not `k * ...`: the loop's 64-bit index as a float is a helper the game's code lacks) */
        Ps_Pill(sNs.vote[k] == NS_V_DONE ? TBO_KIT_SEL : TBO_KIT_BAR, NS_VOTE_X0, y, 498.0f, y + rh, 0x80);
        Ps_Rect(NS_VOTE_X0 + 12.0f, y + rh * 0.25f, NS_VOTE_X0 + 15.0f, y + rh * 0.75f, Ns_Color(k), Ns_Color(k));
    }
    MsgWin_PortDrawString(NULL);
}

static void Ns_DrawVoteText(void) {
    char line[200];
    char what[80];
    char buf[24];
    f32 x, y, cw, ch;
    f32 rh = Ns_VoteRowH();
    f32 big = rh < 30.0f ? 0.3f : 0.36f;
    s32 k;

    Ps_Heading(44.0f, Ps_TextTop(32.0f, 1.3f), NX(NX_VOTE_TITLE), 1.3f, 300.0f, 0x80);
    Ps_Text(20.0f, Ps_TextTop(72.0f, 0.5f), NX(NX_VOTE_RULE), 0.5f, 470.0f, 2);
    for (k = 0; k < sNs.humans && sNs.phase == 1; k++) { /* each cursor's number */
        if (!Ns_Gone(k)) {
            Ns_StageCell(sNs.stg[k], &x, &y, &cw, &ch);
            sprintf(what, "%d", (int)(k + 1));
            Ps_Text(x + 4.0f + 8.0f * (f32)Ns_Before(k), Ps_TextTop(y + 8.0f, 0.36f), what, 0.36f, 0.0f,
                    sNs.vote[k] == NS_V_DONE ? 5 : 1);
        }
    }
    y = NS_CARDS_TOP - rh - 3.0f;
    for (k = 0; k < sNs.humans; k++) {
        const char *name = Port_NetName(k);

        y += rh + 3.0f; /* (as in Ns_DrawVote) */
        if (Ns_Gone(k)) {
            sprintf(what, "%d  %s", (int)(k + 1), NX(NX_CPU));
        } else if (name[0] != 0) {
            sprintf(what, "%d  %s", (int)(k + 1), name);
        } else {
            sprintf(what, NX(NX_PLAYER), (int)(k + 1));
        }
        Ps_Text(NS_VOTE_X0 + 20.0f, Ps_TextTop(y + rh * 0.3f, big), what, big, 70.0f, 0);
        if (Ns_Gone(k)) {
            continue;
        }
        Ps_Align(2);
        Ps_Text(490.0f, Ps_TextTop(y + rh * 0.3f, big), Ns_StageName(sNs.stages[sNs.stg[k]]), big, 100.0f,
                sNs.vote[k] == NS_V_STAGE ? 1 : 3);
        if (sNs.vote[k] == NS_V_MUSIC) {
            sprintf(what, "<  %s  >", Ns_SongName(sNs.songs[sNs.song[k]], buf));
        } else {
            sprintf(what, "%s: %s", NX(NX_MUSIC), Ns_SongName(sNs.songs[sNs.song[k]], buf));
        }
        Ps_Text(490.0f, Ps_TextTop(y + rh * 0.72f, big), what, big, 160.0f,
                sNs.vote[k] == NS_V_DONE ? 5 : sNs.vote[k] == NS_V_MUSIC ? 1 : 3);
        Ps_Align(0);
    }
    if (sNs.phase == 2) {
        sprintf(line, NX(NX_RESULT), Ns_StageName(sNs.stage), Ns_SongName(sNs.bgm, buf));
        Ps_MsgLines(line, 40.0f, 343.0f, 380.0f, 3);
    } else {
        Ps_MsgLines(NX(NX_VOTE_HELP), 40.0f, 343.0f, 380.0f, 3);
    }
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

/* ---- online bots (BT3_NET_BOT, port/src/gs/net.c): a copy whose player the PC plays ---- */
s32 PortNsel_On(void) {
    return sNs.running;
}

/* The PS2 buttons (raw bits, 1 = pressed) that bring player k to character `want` (-1: whatever the cursor is on)
   and make it ready, a press every 8 blanks (each held 4). The way: the grid's row, then its column, cross; the
   form, cross; the costume as it is, cross. */
u32 PortNsel_BotButtons(s32 k, s32 want, u32 tick) {
    enum { B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80, B_CROSS = 0x4000 };
    s32 i;
    s32 cell = -1;
    s32 form = 0;

    if (!sNs.running || k < 0 || k >= sNs.players || tick % 8 >= 4 || !ColorFade_IsInDone()) {
        return 0;
    }
    for (i = 0; want >= 0 && i < sNs.cellCount && cell < 0; i++) {
        s32 f;

        if (sNs.grid[i].id == want) {
            cell = i;
        }
        for (f = 0; f < sNs.grid[i].formCount && f < NS_REEL; f++) {
            if (sNs.grid[i].form[f] == want) {
                cell = i;
                form = f;
            }
        }
    }
    if (sNs.phase != 0) {
        return B_CROSS; /* (the stage and music its cursors are on) */
    }
    switch (sNs.step[k]) {
    case NS_SIDE:
        return B_CROSS; /* (the team it was given) */
    case NS_GRID:
        if (cell < 0 || cell == sNs.col[k] + sNs.row[k] * MCHR_COLS) {
            return B_CROSS;
        }
        if (cell / MCHR_COLS != sNs.row[k]) {
            return cell / MCHR_COLS > sNs.row[k] ? B_DOWN : B_UP;
        }
        return cell % MCHR_COLS > sNs.col[k] ? B_RIGHT : B_LEFT;
    case NS_FORM:
        return sNs.form[k] == form ? B_CROSS : B_RIGHT;
    case NS_COSTUME:
        return B_CROSS;
    }
    return 0;
}

/* From the session's start (or the last battle) until everyone is ready: 1 the battle is set up, 0 the host left. */
s32 PortNsel_Run(void) {
    s32 k;
    s32 keep = sNs.players == Port_NetFighters() && sNs.humans == Port_NetPlayers(); /* (a rematch: the same choices) */

    sNs.players = Port_NetFighters();
    sNs.humans = Port_NetPlayers();
    sNs.ffa = Port_NetFfa();
    Ns_WinInit();
    if (!keep) {
        sNs.team1 = (sNs.players + 1) / 2;
        sNs.cpu = gSaveData->rule[1] >= 0 && gSaveData->rule[1] <= 4 ? gSaveData->rule[1] : 2; /* (the host's rules' level) */
        for (k = 0; k < NS_MAX; k++) { /* (each on a cell of their own to begin with) */
            sNs.col[k] = k % MCHR_COLS;
            sNs.row[k] = k / MCHR_COLS * 2;
            sNs.form[k] = 0;
            sNs.costume[k] = 0;
            sNs.team[k] = k == 0 || (k >= 2 && k <= sNs.team1) ? 0 : 1; /* (place k fighter k unless a player moves) */
            sNs.stg[k] = sNs.stageCount - 1; /* (the vote's cursors on "random") */
            sNs.song[k] = sNs.songCount - 1;
        }
    }
    sNs.phase = 0;
    for (k = 0; k < NS_MAX; k++) {
        ChrGrid_FixCursor(sNs.grid, &sNs.col[k], &sNs.row[k], sNs.rows);
        sNs.step[k] = Ns_First();
        if (k >= sNs.humans) { /* the computer's place: on the grid's "random" cell, ready */
            s32 c;

            for (c = 0; c < sNs.cellCount && sNs.grid[c].id != MCHR_RANDOM; c++) {
            }
            if (c < sNs.cellCount) {
                sNs.col[k] = c % MCHR_COLS;
                sNs.row[k] = c / MCHR_COLS;
            }
            sNs.step[k] = NS_READY;
        }
    }
    sNs.allReady = 0;
    sNs.frames = 0;
    sNs.running = 1;
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
        if (sNs.phase == 0) {
            Ns_Draw();
            Ns_DrawText();
        } else {
            Ns_DrawVote();
            Ns_DrawVoteText();
        }
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
    sNs.running = 0;
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
