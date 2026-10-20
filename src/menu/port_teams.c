#include "common.h"
#include "menu/menu_e.h"
#include "sys/pad.h"

#ifdef PORT
/*
 * PC only: "Team Battle", the settings of a team battle (multi-fighter). The Duel menu's Team opens it (Duel_Main,
 * menu_g_b.c) and "Choose Fighters" goes on to the game's own team character select, where the size of each team
 * (1 to 5) and the fighters are chosen; triangle goes back to the Duel menu.
 *   Mode            Original (one member of each team at a time, the game's own) / Everyone on the field / Free for all
 *   Team 1 health   the health each member of the team starts with, 25..400 %: for teams of different sizes
 *   Team 2 health
 *   Choose Fighters
 * The values are the settings of F1 > Misc (port/src/multi_fighter.c): a change here is one there, saved at once.
 * Tenkaichi Budokai Online's look (menu/port_tbo.c) with the kit of menu/port_screen.c: the Duel menu's heading window
 * and message box (from gMenuArc5, which Duel_Main has loaded), rows of label and value, the pad's prompts. Its words
 * are English or Spanish (Port_MenuLang, F1 > UI), in the screens' TrueType letters (which have the accents the game's
 * font may lack), and said to the screen reader. Offline only: an online session has no Duel menu (the host's
 * settings travel in its rules, gs/net.c).
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
extern int Port_PageAspectMilli(void);              /* port/src/plat_stub.c */
extern void PortA11y_Say(const char *text, int flags); /* port/src/gs/a11y.c: 1 = interrupt what is being said */
extern int Port_MenuLang(void);                      /* port/src/gs/ui.cpp: 0 English, 1 Spanish */
extern int PortMulti_Setting(void);                  /* port/src/multi_fighter.c */
extern void PortMulti_SettingSet(int on);
extern int PortMulti_FfaSetting(void);
extern void PortMulti_FfaSettingSet(int on);
extern int PortMulti_HandicapSetting(int team);
extern void PortMulti_HandicapSet(int team, int percent);

enum { PT_MODE, PT_HP0, PT_HP1, PT_GO, PT_ROWS };
enum { PT_ORIGINAL, PT_ALL, PT_FFA };

/* The words, English and Spanish: UTF-8, written as octal escapes (the ptr32 step of the PC build takes the
   source's bytes for its characters: a letter beyond ASCII in it shifts what the step rewrites after it). */
enum {
    TX_TITLE, TX_MODE, TX_HP0, TX_HP1, TX_GO, TX_ORIGINAL, TX_ALL, TX_FFA, TX_HELP_ORIGINAL, TX_HELP_ALL, TX_HELP_FFA,
    TX_HELP_HP, TX_HELP_HP_OFF, TX_HELP_GO, TX_CHANGE, TX_NEXT, TX_OK, TX_BACK, TX_COUNT
};
static const char *const kPtText[TX_COUNT][2] = {
    {"TEAM BATTLE", "BATALLA POR EQUIPOS"},
    {"Mode", "Modo"},
    {"Team 1 health", "Vida del equipo 1"},
    {"Team 2 health", "Vida del equipo 2"},
    {"Choose Fighters", "Elegir personajes"},
    {"Original", "Original"},
    {"Everyone on the field", "Todos al campo"},
    {"Free for all", "Todos contra todos"},
    {"As in the original game: each team fights one member at a time, and the next one comes in when one falls.",
     "Como en el juego original: cada equipo pelea con un miembro a la vez, y entra el siguiente cuando cae uno."},
    {"Every member of both teams fights at once. The team left standing wins.",
     "Todos los miembros de los dos equipos pelean a la vez. Gana el equipo que queda en pie."},
    {"Everyone on the field and on their own, with no teammates. The last one standing wins.",
     "Todos en el campo y cada uno por su cuenta, sin compa\303\261eros. Gana el \303\272ltimo en pie."},
    {"The health each member of team %d starts with. Over or under 100 %% evens out teams of different sizes.",
     "La vida con la que empieza cada miembro del equipo %d. M\303\241s o menos de 100 %% equilibra equipos de distinto tama\303\261o."},
    {"Only for Everyone on the field and Free for all.", "Solo para Todos al campo y Todos contra todos."},
    {"Choose how many fight on each team (1 to 5) and their fighters. Then the battle starts.",
     "Elige cu\303\241ntos pelean en cada equipo (de 1 a 5) y sus personajes. Despu\303\251s empieza la batalla."},
    {"Change", "Cambiar"},
    {"Next", "Siguiente"},
    {"OK", "Aceptar"},
    {"Back", "Volver"},
};
#define TX(id) ((char *)kPtText[id][sPt.lang])

typedef struct PortTeamsScreen {
    s32 cur;
    s32 mode;
    s32 hp[2];
    s32 lang;
    s32 frames;
    s32 leaving;
    s32 result;
    void *winRes; /* the Duel menu's screen unpacked: its windows' data */
} PortTeamsScreen;
static PortTeamsScreen sPt;

static void Pt_ModeSave(void) {
    PortMulti_SettingSet(sPt.mode != PT_ORIGINAL);
    PortMulti_FfaSettingSet(sPt.mode == PT_FFA);
}

static void Pt_Leave(s32 result) {
    sPt.leaving = 1;
    sPt.result = result;
    ColorFade_StartOut(0, 0, 0, 0x14);
}

static void Pt_Update(s32 live) {
    Pad *pad = &gPad[0];
    s32 side;

    if (sPt.cur != PT_GO) {
        Ps_Prompt(3, TX(TX_CHANGE));
    }
    Ps_Prompt(0, sPt.cur == PT_GO ? TX(TX_OK) : TX(TX_NEXT));
    Ps_Prompt(2, TX(TX_BACK));
    if (!live) {
        return;
    }
    Ps_Move(&sPt.cur, PT_ROWS);
    side = Ps_Side();
    if (side != 0 && sPt.cur == PT_MODE) {
        sPt.mode = Ps_Wrap(sPt.mode + side, PT_ORIGINAL, PT_FFA);
        Pt_ModeSave();
        Snd_PlaySe(1, 0);
    } else if (side != 0 && (sPt.cur == PT_HP0 || sPt.cur == PT_HP1)) {
        s32 t = sPt.cur - PT_HP0;
        s32 hp = sPt.hp[t] + side * 25;

        if (sPt.mode == PT_ORIGINAL || hp < 25 || hp > 400) {
            Snd_PlaySe(1, 7); /* (the Duel menu's "not now") */
        } else {
            sPt.hp[t] = hp;
            PortMulti_HandicapSet(t, hp);
            Snd_PlaySe(1, 0);
        }
    }
    if (pad->gamePressed & (PADG_CROSS | PADG_START)) {
        if (sPt.cur == PT_GO || (pad->gamePressed & PADG_START)) {
            Snd_PlaySe(1, 1);
            Pt_Leave(1);
        } else {
            Snd_PlaySe(1, 0);
            sPt.cur++;
        }
    } else if (pad->gamePressed & PADG_TRIANGLE) {
        Snd_PlaySe(1, 2);
        Pt_Leave(0);
    }
}

static char *Pt_Value(s32 row, char *buf) {
    switch (row) {
    case PT_MODE:
        return TX(TX_ORIGINAL + sPt.mode);
    case PT_HP0:
    case PT_HP1:
        if (sPt.mode == PT_ORIGINAL) {
            return "\342\200\224";
        }
        sprintf(buf, "%d %%", (int)sPt.hp[row - PT_HP0]);
        return buf;
    }
    return "";
}

/* The line about the row under the cursor (for the box and the screen reader). */
static char *Pt_Help(char *buf) {
    switch (sPt.cur) {
    case PT_MODE:
        return TX(TX_HELP_ORIGINAL + sPt.mode);
    case PT_HP0:
    case PT_HP1:
        if (sPt.mode == PT_ORIGINAL) {
            return TX(TX_HELP_HP_OFF);
        }
        sprintf(buf, TX(TX_HELP_HP), (int)(sPt.cur - PT_HP0 + 1));
        return buf;
    }
    return TX(TX_HELP_GO);
}

/* Says what changed: the title and the row as the screen opens, then the row and its value, then the line. */
static void Pt_Speak(void) {
    static char said[320];
    char now[320];
    char value[16];
    char help[200];
    s32 k;

    now[0] = 0;
    Ps_Cat(now, TX(TX_MODE + sPt.cur), 320);
    if (sPt.cur != PT_GO) {
        Ps_Cat(now, ": ", 320);
        Ps_Cat(now, sPt.cur != PT_MODE && sPt.mode == PT_ORIGINAL ? "-" : Pt_Value(sPt.cur, value), 320);
    }
    Ps_Cat(now, ". ", 320);
    Ps_Cat(now, Pt_Help(help), 320);
    for (k = 0; now[k] == said[k] && now[k] != 0; k++) {
    }
    if (sPt.frames == 0) { /* (the screen opens: its title first) */
        char open[400];

        Ps_Copy(said, now, 320);
        Ps_Copy(open, TX(TX_TITLE), 400);
        Ps_Cat(open, ". ", 400);
        Ps_Cat(open, now, 400);
        PortA11y_Say(open, 1);
    } else if (now[k] != said[k]) {
        Ps_Copy(said, now, 320);
        PortA11y_Say(now, 1);
    }
}

/* The tournament grounds over the whole window, as Tenkaichi Budokai Online's (Tb_Backdrop). */
static void Pt_Backdrop(void) {
    f32 aspect = (f32)Port_PageAspectMilli() / 1000.0f;
    f32 pic = Ps_PicW(TBO_BG) / Ps_PicH(TBO_BG);
    f32 u = aspect < pic ? aspect / pic : 1.0f;
    f32 v = aspect < pic ? 1.0f : pic / aspect;

    Ps_SpriteScreen(TBO_BG, 0.0f, 0.0f, 512.0f, 448.0f, 0.5f - u * 0.5f, 0.5f - v * 0.5f, 0.5f + u * 0.5f, 0.5f + v * 0.5f, PS_PLAIN, 0x80);
}

#define PT_ROW_Y 100.0f
#define PT_ROW_STEP 34.0f
#define PT_BOX_TOP 326.0f /* the top edge of the game's message box (MsgWin) */

static void Pt_Draw(void) {
    s32 i;

    /* the hosts on the box's line, half behind it (as Tenkaichi Budokai Online's), cheering */
    Ps_Stand(TBO_HOSTS_CHEER, 408.0f, PT_BOX_TOP + 150.0f, 300.0f, 0x80);
    Ps_Panel(TBO_KIT_WINDOW, 14.0f, PT_ROW_Y - 14.0f, 316.0f, PT_ROW_Y + PT_ROW_STEP * PT_ROWS + 6.0f, 0x80);
    for (i = 0; i < PT_ROWS; i++) {
        f32 y = PT_ROW_Y + PT_ROW_STEP * (f32)i;

        if (i == sPt.cur) {
            Ps_Pill(TBO_KIT_SEL, 20.0f, y - 2.0f, 310.0f, y + 28.0f, 0x80);
            if (i != PT_GO) {
                Ps_Button(3, 294.0f, y + 13.0f, 20.0f);
            }
        } else {
            Ps_Pill(TBO_KIT_BAR, 24.0f, y, 306.0f, y + 26.0f, 0x80);
        }
    }
    MsgWin_PortDrawString(NULL); /* the game's box; its line is ours, in the screens' letters (Pt_DrawText) */
}

static void Pt_DrawText(void) {
    char value[16];
    char help[200];
    s32 i;

    Ps_Heading(44.0f, Ps_TextTop(32.0f, 1.3f), TX(TX_TITLE), 1.3f, 300.0f, 0x80);
    for (i = 0; i < PT_ROWS; i++) {
        f32 row = PT_ROW_Y + PT_ROW_STEP * (f32)i + 13.0f;
        s32 off = (i == PT_HP0 || i == PT_HP1) && sPt.mode == PT_ORIGINAL;

        Ps_Text(36.0f, Ps_TextTop(row, 0.66f), TX(TX_MODE + i), 0.66f, 150.0f, off ? 4 : i == sPt.cur ? 1 : 0);
        if (i != PT_GO) {
            Ps_Align(2);
            Ps_Text(274.0f, Ps_TextTop(row, 0.54f), Pt_Value(i, value), 0.54f, 120.0f, off ? 4 : 3);
            Ps_Align(0);
        }
    }
    /* the line about the row, three lines at most (up to 400 units: the hosts stand to the right) */
    Ps_MsgLines(Pt_Help(help), 40.0f, 343.0f, 360.0f, 3);
}

static void Pt_WinInit(void) {
    void *icons;

    sPt.winRes = Sprite_Unpack(MPACK_AT(gMenuArc5, 1), NULL, NULL); /* (the Duel menu's own: DuelMenu_Run(1)) */
    icons = MPACK_AT(sPt.winRes, 24);
    Res_RelocateOffsets(&icons, icons, icons);
    IconWin_Init(MPACK_AT(sPt.winRes, 23), icons);
    IconWin_PortHideWords(); /* "Versus Menu" and its emblem: the screen's own title is written there */
    IconWin_Open();
    MsgWin_Init(MPACK_AT(sPt.winRes, 22), MPACK_AT(sPt.winRes, 21), 1, 0);
    MsgWin_SetText(NULL);
    MsgWin_PortSameTop();
    MsgWin_Open();
}

static void Pt_WinTerm(void) {
    MsgWin_Term();
    IconWin_Term();
    Heap_Free(sPt.winRes);
    sPt.winRes = NULL;
}

/* From the Duel menu's Team until a choice: 1 "Choose Fighters" (the team character select next), 0 back. */
s32 PortTeams_Run(void) {
    memset(&sPt, 0, sizeof(sPt));
    sPt.mode = !PortMulti_Setting() ? PT_ORIGINAL : PortMulti_FfaSetting() ? PT_FFA : PT_ALL;
    sPt.hp[0] = PortMulti_HandicapSetting(0);
    sPt.hp[1] = PortMulti_HandicapSetting(1);
    sPt.lang = Port_MenuLang() == 1;
    sPt.cur = PT_GO; /* (the last row: a player who keeps the settings presses cross once) */
    Pt_WinInit();
    ColorFade_StartIn(0, 0, 0, 0x14);
    while (1) {
        Job_Run();
        Gfx_BeginFrame();
        Pad_Update();
        Snd_Update();
        ColorFade_Update();
        Ps_PromptsClear();
        Pt_Update(!sPt.leaving && ColorFade_IsInDone());
        Pt_Speak();
        Ps_PageSet();
        Pt_Backdrop();
        IconWin_Draw(); /* the clouds in the corners, the line under the title */
        Pt_Draw();
        Pt_DrawText();
        Ps_PromptsDraw();
        ColorFade_Draw();
        File_Stub264D90();
        Gfx_EndFrame(1);
        Dma_Flush();
        sPt.frames++;
        if (sPt.leaving && ColorFade_IsOutDone()) {
            break;
        }
    }
    sceGsSyncPath(0, 0);
    Pt_WinTerm();
    Dma_ResetBuffers();
    return sPt.result;
}
#endif
