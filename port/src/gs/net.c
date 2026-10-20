/*
 * Online play, first step: two copies of the game kept in step by exchanging the pads' input (branch netplay).
 *
 * Both copies run the same game from the first vertical blank on. Each has ONE local player (the host is player 1,
 * the one who joins player 2); at every vertical blank a copy sends its player's input for that blank to the other
 * and does not go on before it has the other's input for it ("lockstep"). The game then reads the two pads from
 * here on both machines, so both simulate the same thing. Nothing is predicted and nothing is rewound yet: a slow
 * connection makes both games wait. Rewinding (port/src/gs/state.c) comes on top of this.
 *
 *   BT3_NET_HOST=<port>             wait for the other copy on this UDP port, be player 1
 *   BT3_NET_JOIN=<address>:<port>   connect to a host, be player 2
 *   BT3_NET_DELAY=<n>               a player's input takes effect n blanks after it was made (default 2): the time
 *                                   the other side has to receive it without the game having to wait
 *   BT3_NET_SCRIPT=<file>           testing: the local player's input comes from a table (36 bytes per blank: pad
 *                                   1, pad 2; written by BT3_PAD_TABLE=<file> during an ordinary run), not from
 *                                   the keyboard or a controller
 *   BT3_NET_LOSS=<percent>, BT3_NET_LAG=<ms>   testing: drop that share of the packets sent / hold each one back
 *   BT3_VIEW=0|1                    testing: show that player's view full screen in a two-player battle (what a
 *                                   connected copy does with its own player's)
 *
 * A packet carries the sender's input for the last 16 blanks, so a lost packet is covered by the next ones; while
 * a copy waits it sends its own latest packet again every few milliseconds.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define closesocket close
#endif
#include "gs_internal.h"

extern int Port_PadRead(int socket, unsigned char *data); /* gs_input.c: the keyboard and the controllers */
void Port_NetLeave(void);

enum { PAD = 18, RING = 4096, REDUNDANT = 16, MAGIC = 0x4E335442 /* "BT3N" */ };
enum { T_HELLO = 1, T_HELLO_ACK = 2, T_INPUT = 3, T_BYE = 4, T_PING = 5, T_PONG = 6, T_CONFIG = 7, T_CONFIG_ACK = 8,
       T_PUNCH = 9 /* nothing to read: sent so that the sender's router lets the other side's packets in (room codes) */,
       T_CHAT = 10 /* a line of text between the players (net_chat.c): its number, its length, its bytes. Not part of
                      the game: a copy without the chat ignores it, so NET_VERSION stays */,
       T_WAIT = 11 /* the host's lobby to a player who greeted it while the room is not full yet: how many are there,
                      of how many (Port_LobbyPoll) */ };
/* The version of what goes over the line. The greeting and its answer carry it, and two copies that differ do not
   start a match (the input packets changed with version 2: times for the ping were added; a copy that read them
   the old way took them for input). 1 was releases 0.1.8 and 0.1.9, which said no version. */
enum { NET_VERSION_BASE = 6, IN_HEAD = 44 }; /* 3 (0.1.15): the host's battle rules travel with its choices.
   4 (0.1.34): a session's start-up no longer follows the player's "Skip logos" / "Skip intro video" (0.1.32 and
   0.1.33 with either on computed another start-up than the other side and the two never agreed again: such a copy
   must not be let into a match); the input packets carry the sender's check value (sync_*, below), 12 bytes more
   in their head; the goodbye can say why.
   5 (multi-fighter): the rules word carries the host's "every member on the field" and each team's starting health
   (RULE_MULTI, below): a copy that did not read them would set up another battle than the host's.
   6 (N players, SPEC-netplay-n.md): up to ten in a match, the player's number in the input packets' type word, the
   host's answers say each one's number and how many play; rooms for more than two (T_WAIT); kGone. */
static int sVersionBad; /* the other side answered with another version */
extern int gPortBt4; /* plat_mem.c: the BT4 disc (PRIVATE) */
/* BT4 computes another game: its copies meet only each other (0x400 apart, in the greeting and at the service). */
#define NET_VERSION ((uint32_t)(gPortBt4 ? NET_VERSION_BASE + 0x400 : NET_VERSION_BASE))
static int version_is(const uint8_t *p) {
    uint32_t v = NET_VERSION;
    return memcmp(p, &v, 4) == 0;
}
/* The system save's folder and icon on the memory card (sys/mcflow_a.c, stgm_a_b.c): BT4's have its own names. */
#define NET_SAVE_DIR (gPortBt4 ? "BASLUS-21978DBZT4" : "BASLUS-21678DBZT3")
#define NET_SAVE_ICO (gPortBt4 ? "dbzs4.ico" : "dbzsm.ico")
static void remove_session_save(void) {
    char path[160];
    snprintf(path, sizeof(path), "net_session/card1/%s/%s", NET_SAVE_DIR, NET_SAVE_DIR);
    remove(path);
    snprintf(path, sizeof(path), "net_session/card1/%s/icon.sys", NET_SAVE_DIR);
    remove(path);
    snprintf(path, sizeof(path), "net_session/card1/%s/%s", NET_SAVE_DIR, NET_SAVE_ICO);
    remove(path);
}
#include "net_match.h"
#include "net_relay.h"
#include "net_chat.h"
static int sLobbyOther;

/* What the meter shows (ui.cpp): the round trip time, and per second how often the game went back, how far, and
   how often and for how long it waited for the other player. */
static uint32_t sEchoTs[10], sEchoAtMs[10]; /* by player: its last send time and when it arrived here (NMAX below) */
static int sPingMs = -1;
static unsigned sSecRoll, sSecRollTicks, sSecWaits, sSecWaitMs, sSecTicks;
static int sShown[5] = {0, 0, 0, 0, 0}; /* ping, rollbacks, depth x 10, waits, ms waited: of the last second */

static uint32_t now_ms(void) {
    return (uint32_t)(SDL_GetTicksNS() / 1000000ull) | 1u; /* (never 0: 0 means "no time yet") */
}

/* What the host chose in the Dragon Net Battle window (-1: not said; the environment or the defaults decide).
   The host's answer to the greeting carries its input delay and rollback limit, and the joining side takes them:
   the delay has to be the same on both sides. (The rollback limit is each side's own business, the two ways of
   playing work against each other; the host's is only the other side's starting value.) */
static int sOptRoll = -1, sOptDelay = -1;
static int sCfgGot, sCfgDelay, sCfgRoll;
static int sGone; /* the other player said goodbye (2: because its check found the two games apart) */
extern void Port_UiNotice(const char *text); /* ui.cpp: a line over the picture for a few seconds */

static int sAutoDelay;
static int sOptAuto;      /* the host asked for the input delay to be chosen from the line's ping */
static int sSetupRtt = -1; /* the smallest round trip measured while setting up, ms (with more players: the worst
                              player's smallest) */
static int sRttOf[10];     /* (by player: its smallest; NMAX below) */
static int sCfgAcked;

void Port_TestSlow(int what, int amount);

void Port_NetOptions(int rollback, int delay) {
    sOptRoll = rollback;
    sOptAuto = delay == -2; /* -2: automatic; -1: not said; else the delay in blanks */
    sOptDelay = delay >= 0 ? delay : -1;
}

#define NAME_LEN 20
static char sNames[10][NAME_LEN], sMyName[NAME_LEN] = "Player"; /* (by player, NMAX below) */

void Port_NetNameSet(const char *name) {
    snprintf(sMyName, sizeof(sMyName), "%s", name != NULL && name[0] != '\0' ? name : "Player");
}

/* A match found with a room code (the lobby, below): the local port the lobby's socket had, which the session's
   socket takes again, and for the host the address the other player's greeting came from. */
static int sCodeLocalPort, sCodePunchSet;
/* The lobby's socket, handed to the session that follows (a match found with a room code, begun without a restart).
   It is not closed and opened again on the same port: while it is closed, packets from the other side meet a
   closed port, the system answers "unreachable", and a relay stops passing that player's packets on (seen with
   Cloudflare's: the match's greeting never arrived). It also keeps the routers' view of the conversation whole. */
static int sHandSock = -1, sFromLobby;
int Port_LobbyRelayed(void);
static struct sockaddr_in sCodePunch;

/* The match's rules, chosen by the host in the lobby window (the game's own versus menu is not shown in a session):
   the battle type as the versus menu leaves it (0 single, 1 team, 2 DP battle), the row of the DP limit list
   (0..2: 10, 15, 20) and the time limit as the battle settings keep it (save rule 0: 0..4 = 60, 90, 180, 240
   seconds, none). One word: type | dp << 4 | time << 8. The host's travels with its other choices (T_HELLO_ACK,
   T_CONFIG); headless.c applies it on both sides when the session enters the versus mode. */
static uint32_t sRules = 3u << 8; /* single battle, 240 seconds: the game's defaults */

/* The match's seed. A session is a fresh start of the program on both sides with the same fixed seed (plat_sys.c),
   so the game's random numbers were the same in every session: "Random" at the character select gave the same
   fighters in the same order every time (reported: Yajirobe, then Cooler). The host draws a number when it starts,
   it travels behind the host's other choices (T_HELLO_ACK, T_CONFIG), and both copies seed the game's generator
   with it at the same point, where the session enters the versus mode (headless.c). 0 = leave the generator as it
   is. BT3_NET_SEED=<n> on the host: that number (tests; 0 for the sequence every session had before). */
static uint32_t sSeed;

unsigned Port_NetSeed(void) {
    return sSeed;
}

static void seed_draw(void) {
    const char *e = getenv("BT3_NET_SEED");
    if (e != NULL) {
        sSeed = (uint32_t)strtoul(e, NULL, 0);
    } else {
        do {
            sSeed = SDL_rand_bits() ^ (uint32_t)SDL_GetTicksNS() ^ (uint32_t)(SDL_GetPerformanceCounter() >> 7);
        } while (sSeed == 0);
    }
}

/* Each player's own two battle options of the game's option menu (save 0x1694: the camera type; 0x1698: the
   screen shake; they become BattleOption.optA / optB of the player's side, battle_load.c). A session starts from
   an empty save on both sides, so both sides had the defaults. Each copy now reads its player's own from his save
   and tells the other; both then set side 0 to the host's and side 1 to the joiner's, the same on both copies
   (the cameras are part of the game's state). Sent behind the name in the greeting and in its answers, with a
   mark: a copy of 0.1.25 or earlier does not send them and does not read them, and then neither side uses them.
   OPT_MARK | camera | shake << 8. */
int Port_NetActive(void);
#define OPT_MARK 0x4F500000u
static uint32_t sOpts[10]; /* by player: 0 = not known (NMAX below) */

static uint32_t own_options(void) {
    const char *root = getenv("BT3_SAVES") != NULL ? getenv("BT3_SAVES") : "saves";
    const char *carried = getenv("BT3_NET_OPTS"); /* (across the restart into the session: the save folder is the session's by then) */
    char path[600];
    unsigned char w[8];
    uint32_t a = 0, b = 0;
    FILE *fp;

    if (carried != NULL) {
        return OPT_MARK | ((uint32_t)strtoul(carried, NULL, 0) & 0xFFFFu);
    }
    snprintf(path, sizeof(path), "%s/card1/%s/%s", root, NET_SAVE_DIR, NET_SAVE_DIR);
    fp = fopen(path, "rb");
    if (fp != NULL) {
        if (fseek(fp, 0x1694, SEEK_SET) == 0 && fread(w, 1, 8, fp) == 8) {
            a = w[0] | (uint32_t)w[1] << 8 | (uint32_t)w[2] << 16 | (uint32_t)w[3] << 24;
            b = w[4] | (uint32_t)w[5] << 8 | (uint32_t)w[6] << 16 | (uint32_t)w[7] << 24;
        }
        fclose(fp);
    }
    return OPT_MARK | (a > 15 ? 0 : a) | (b > 15 ? 0 : b) << 8; /* (anything else: the defaults) */
}

static void opts_take(int player, const uint8_t *at) {
    uint32_t v;
    memcpy(&v, at, 4);
    if ((v & 0xFFFF0000u) == OPT_MARK) {
        sOpts[player] = v;
    }
}

/* For the game (BattleSetup_DefaultOptions): the two sides' options of an online match. 0 = not both known. */
int Port_NetSideOptions(int *camera, int *shake) {
    int i;
    if (!Port_NetActive() || sOpts[0] == 0 || sOpts[1] == 0) {
        return 0;
    }
    for (i = 0; i < 2; i++) {
        /* (the other side's word is whatever its program sent: only the values the options menu can set go into
           the game, the camera distance 0..2 and the shake on or off; anything else is the default) */
        camera[i] = (int)(sOpts[i] & 0xFF) > 2 ? 0 : (int)(sOpts[i] & 0xFF);
        shake[i] = (int)(sOpts[i] >> 8 & 0xFF) > 1 ? 0 : (int)(sOpts[i] >> 8 & 0xFF);
    }
    {
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "bt3: net: a battle is set up with each side's own camera type and screen shake\n");
        }
    }
    return 1;
}

/* The match's speed, a percentage of the game's own (100 to 200). It is the vertical blanks' pace (plat_stub.c)
   and nothing of the game's state: every step is computed as always, there are just more of them a second; the
   sound keeps its pitch (snd_adx.c). The host's choice, like the other rules, in the rules word:
     bits 16..23  the percentage (0 = not said)
     bit 12       "120%", which is all 0.1.29 knows of this; set beside the number when the number is 120
   What the one who joins knows is in the capability word of its greeting (CAP_MARK | 1: bit 12; | 2: the number;
   0.1.28 and earlier send none). The host takes back what the other does not know before it answers, so the two
   never run at different paces: a 0.1.29 gets 120% or 100%, anything older 100%.
   NET_SPEED_STANDARD is what a quick match runs at and what the Host tab starts from. Competitive play of this
   game is "120% on the PAL version in Dolphin"; on this (the USA) version's scale the players settled on 114%
   after trying the sliders of 0.1.30 (told by the maintainer, 2026-10-12). */
#define RULE_FAST 0x1000u
#define RULE_SPEED_SHIFT 16
#define CAP_MARK 0x43410000u
#define NET_SPEED_STANDARD 114
static int sWantSpeed = NET_SPEED_STANDARD;

static uint32_t speed_bits(int percent) {
    return (uint32_t)percent << RULE_SPEED_SHIFT | (percent == 120 ? RULE_FAST : 0);
}

/* Several fighters on the field (multi-fighter T1.9): the host's "Team battle: every member on the field" and each
   team's starting health, in the rules word too, so that both copies set up the same battle. Only a hosted room has
   them (Port_NetMultiRules, the Host tab); a quick match's rules leave them out (standard battles).
     bit 13       every member on the field
     bit 14       free for all: every fighter on its own
     bits 24..27  team 1's starting health: (percent / 25 - 1) ^ 3, so that 0 is 100 % (25..400 %, steps of 25)
     bits 28..31  team 2's */
#define RULE_MULTI 0x2000u
#define RULE_FFA 0x4000u /* bit 14: and every fighter on its own (free for all) */

static uint32_t health_bits(int percent) {
    return ((uint32_t)(percent / 25 - 1) ^ 3u) & 15u;
}

void Port_NetMultiRules(void) {
    extern int PortMulti_Setting(void);                /* port/src/multi_fighter.c: the player's own settings */
    extern int PortMulti_HandicapSetting(int team);
    extern int PortMulti_FfaSetting(void);

    sRules = (sRules & ~(RULE_MULTI | RULE_FFA | 0xFF000000u)) | (PortMulti_Setting() ? RULE_MULTI : 0) |
             (PortMulti_FfaSetting() ? RULE_FFA : 0) |
             health_bits(PortMulti_HandicapSetting(0)) << 24 | health_bits(PortMulti_HandicapSetting(1)) << 28;
}

/* A room for more than two (Tenkaichi Budokai Online's Create Room): every fighter on the field, one per player, in
   two teams or free for all, each with full health. */
void Port_NetPlayersRules(int ffa) {
    sRules = (sRules & ~(RULE_MULTI | RULE_FFA | 0xFF000000u)) | RULE_MULTI | (ffa ? RULE_FFA : 0);
}

int Port_NetMultiAll(void) {
    return (sRules & RULE_MULTI) != 0;
}

int Port_NetFfa(void) {
    return (sRules & RULE_FFA) != 0;
}

int Port_NetHandicap(int team) {
    int v = (int)(sRules >> (24 + 4 * (team != 0)) & 15u);

    return ((v ^ 3) + 1) * 25;
}

void Port_NetRulesSet(int type, int dp, int time) {
    sRules = (uint32_t)(type < 0 || type > 2 ? 0 : type) | (uint32_t)(dp < 0 || dp > 2 ? 0 : dp) << 4 | (uint32_t)(time < 0 || time > 4 ? 3 : time) << 8 |
             speed_bits(sWantSpeed);
}

void Port_NetSpeedSet(int percent) {
    sWantSpeed = percent < 100 ? 100 : percent > 200 ? 200 : percent;
    sRules = (sRules & ~(RULE_FAST | 0xFFu << RULE_SPEED_SHIFT)) | speed_bits(sWantSpeed);
}

int Port_NetSpeedStandard(void) {
    return NET_SPEED_STANDARD;
}

static int rules_speed(void) {
    int percent = (int)(sRules >> RULE_SPEED_SHIFT & 0xFF);
    return percent >= 100 && percent <= 200 ? percent : (sRules & RULE_FAST) != 0 ? 120 : 100;
}

int Port_NetSession(void);
/* 1000 = the game's own pace; only ever the match's, never the player's own setting. */
int Port_NetSpeedMilli(void) {
    return Port_NetSession() || Port_NetActive() ? rules_speed() * 10 : 1000;
}

void Port_NetRules(int *type, int *dp, int *time) {
    *type = (int)(sRules & 15) > 2 ? 0 : (int)(sRules & 15);
    *dp = (int)(sRules >> 4 & 15) > 2 ? 0 : (int)(sRules >> 4 & 15);
    *time = (int)(sRules >> 8 & 15) > 4 ? 3 : (int)(sRules >> 8 & 15);
}

/* The input delay for a line with this round trip. A blank is 16.7 ms; the other side's input for a blank is
   entered `delay` blanks ahead and takes half the round trip plus up to a blank (it waits for the next look at
   the socket) to arrive, so the game runs about ceil(trip / 2 / 16.7 + 1) - delay blanks on guesses. The delay
   is what keeps that at 4 or under: 1 on a good line (up to about 130 ms), more beyond, 6 at most. */
static int auto_delay(int rttMs) {
    int ticks, delay;
    if (rttMs < 0) {
        return 2;
    }
    {
        int blankUs = 16683 * 1000 / Port_NetSpeedMilli(); /* (a blank is shorter at 120%) */
        ticks = (rttMs * 1000 / 2 + blankUs - 1) / blankUs + 1; /* ceil(one way in blanks) + 1 */
    }
    delay = ticks - 4;
    return delay < 1 ? 1 : delay > 6 ? 6 : delay;
}

/* Players in the match (SPEC-netplay-n.md at the project root): 2 as always, up to NMAX. Player k plays with pad k
   and plays fighter k (port/src/multi_fighter.c, PortMulti_PadOf). Everyone talks to the host only: each sends the
   host its own input, and the host passes each one's on to all the others ("star"). With 2 players that is exactly
   the two-player exchange it always was. The host's number comes from BT3_NET_PLAYERS (tests) or its lobby; the
   others learn it, and their own player number, from the host's answer to their greeting. */
enum { NMAX = 10 };
static int sCount = 2;
static int sLobbyPlayers = 2; /* the host's lobby: how many are to play (Port_LobbyPlayersSet) */
static int sMode = -1; /* 0 off, 1 host, 2 join */
static int sSock = -1, sMe, sDelay = 2, sConnected, sLoss, sLagMs;
static struct sockaddr_in sPeer;            /* the host for the others; for the host, player 1 */
static struct sockaddr_in sPeers[NMAX];     /* the host: player k's address (k >= 1; sPeers[1] is sPeer) */
static int sJoined;                         /* the host: players who have greeted it (1 .. sCount - 1) */
static uint8_t sIn[NMAX][RING][PAD]; /* [player][blank % RING] */
static uint32_t sHave[NMAX];         /* inputs known for blanks below this */
static uint32_t sTick;            /* the blank the game is in */
static FILE *sScript;
static uint64_t sWaitNs, sWaits;

static const uint8_t kIdle[PAD] = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80};
/* NET_GONE: the pad of a player who left a match of more than two (left_fill): idle, with a mark in the last
   pressure byte that makes the computer play their fighter (src/battle/btl_input.c, BtlInput_Sample). */
static const uint8_t kGone[PAD] = {0xFF, 0xFF, 0x80, 0x80, 0x80, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x47};
static uint32_t sLeft; /* the host of more than two: the players who have left (bit k) */

/* rollback (roll_tick) */
extern int Port_RollCan(void), Port_RollSave(void); /* gs/state.c */
extern void Port_RollBack(int k);
extern int gPortResim;
int Port_NetWarp(void);
static int sRollMax, sResim, sRollBegun;
static uint8_t sUsed[NMAX][RING][PAD]; /* what each other player's pad read in each blank */
static uint32_t sChecked;        /* blanks below this were given the other player's real input */
static uint32_t sLive;           /* during a re-run: the blank that was the present when it began */
static unsigned sRollbacks, sRollTicks, sStalls;
/* Keeping the two copies together in time: each says in its packets how many blanks it is beyond the input it
   has from the other (sTheirAhead). On an even line both numbers are the same; the copy whose clock is ahead has
   the larger one, by twice the difference of the clocks. That copy lets its blanks come a little later until the
   two agree (plat_stub.c's pacing grid is moved), so that neither does all the guessing and going back. */
static int32_t sTheirAhead, sMyAhead;
static int sBalance;  /* (mine - theirs), smoothed, in 1/16 blank */
static unsigned sSlowed;
extern unsigned gPortPaceShiftNs;

/* BT3_NET_LATENCY=<ms>: testing, a line that takes that long one way. What is sent waits in a queue. */
static int sLatencyMs;
static struct { uint64_t due; int n; struct sockaddr_in to; uint8_t data[IN_HEAD + 16 * PAD]; } sQueue[256];
static unsigned sQueueHead, sQueueTail;

/* Every packet of a match goes out and comes in through these two: the host of a match found with a room code may
   hold a relay address (net_relay.c), and then the other player's packets can arrive wrapped by the relay and must
   go back the same way. Without a relay they are plain sendto / recvfrom. */
static int sStatOut, sStatIn, sStatInVia, sStatOutVia, sStatErr, sStatOther, sStatBytes;
static void net_stat(void) { /* BT3_RELAY_LOG: packets out and in, a line a second */
    static uint64_t last;
    uint64_t now = SDL_GetTicks();
    if (now - last >= 1000 && getenv("BT3_RELAY_LOG") != NULL) {
        if (sStatOut + sStatIn > 0) {
            fprintf(stderr, "bt3: net: packets out %d (%d through the relay, %d failed, %d bytes), in %d (%d through the relay, %d of the relay's own)\n", sStatOut,
                    sStatOutVia, sStatErr, sStatBytes, sStatIn, sStatInVia, sStatOther);
        }
        sStatOut = sStatIn = sStatInVia = sStatOutVia = sStatErr = sStatOther = sStatBytes = 0;
        last = now;
    }
}

static void net_send(const void *buf, int n, const struct sockaddr_in *to) {
    sStatOut++;
    sStatBytes += n;
    if (Relay_IsPeer(to)) {
        sStatOutVia++;
        Relay_Send(sSock, buf, n, to);
    } else if (sendto(sSock, (const char *)buf, n, 0, (const struct sockaddr *)to, sizeof(*to)) != n) {
        sStatErr++;
    }
}

static int net_recv(uint8_t *pkt, int size, struct sockaddr_in *from) {
    Relay_Tick(sSock);
    net_stat();
    for (;;) {
        socklen_t fromLen = sizeof(*from);
        uint8_t *data = NULL;
        int len = 0, n = (int)recvfrom(sSock, (char *)pkt, size, 0, (struct sockaddr *)from, &fromLen), kind;
        if (n <= 0) {
            return n;
        }
        kind = Relay_Unwrap(pkt, n, from, &data, &len);
        if (kind == 2) {
            Relay_SawDirect(from);
            sStatIn++;
            return n;
        }
        if (kind == 0) {
            sStatOther++;
        }
        if (kind == 1) {
            sStatIn++;
            sStatInVia++;
            memmove(pkt, data, (size_t)len);
            return len;
        }
    }
}

static void queue_flush(void) {
    uint64_t now = SDL_GetTicksNS();
    while (sQueueHead != sQueueTail && sQueue[sQueueHead % 256].due <= now) {
        net_send(sQueue[sQueueHead % 256].data, sQueue[sQueueHead % 256].n, &sQueue[sQueueHead % 256].to);
        sQueueHead++;
    }
}

static void send_to(const struct sockaddr_in *to, const void *buf, int n) {
    if (sLoss != 0 && (rand() % 100) < sLoss) {
        return;
    }
    if (sLatencyMs > 0 && n <= (int)sizeof(sQueue[0].data) && sQueueTail - sQueueHead < 256) {
        sQueue[sQueueTail % 256].due = SDL_GetTicksNS() + (uint64_t)sLatencyMs * 1000000ull;
        sQueue[sQueueTail % 256].n = n;
        sQueue[sQueueTail % 256].to = *to;
        memcpy(sQueue[sQueueTail % 256].data, buf, (size_t)n);
        sQueueTail++;
        return;
    }
    net_send(buf, n, to);
}

/* Player k's address, for the host (k >= 1). */
static const struct sockaddr_in *peer_of(int k) {
    return k == 1 ? &sPeer : &sPeers[k];
}

/* To the other side: the host for the others; for the host, every other player. */
static void send_to_peer(const void *buf, int n) {
    int k;

    if (sMode != 1) {
        send_to(&sPeer, buf, n);
        return;
    }
    for (k = 1; k < sCount; k++) {
        send_to(peer_of(k), buf, n);
    }
}

/* The host: which player an address is (1 .. sCount - 1), or -1. */
static int player_at(const struct sockaddr_in *from) {
    int k;

    for (k = 1; k < sJoined + 1 && k < sCount; k++) {
        if (from->sin_addr.s_addr == peer_of(k)->sin_addr.s_addr && from->sin_port == peer_of(k)->sin_port) {
            return k;
        }
    }
    return -1;
}

/* Do the two copies still compute the same game? Only button presses are exchanged, so nothing else would say:
   two copies that have come apart go on, each with its own fight, for good. Every SYNC_EVERY blanks each copy
   notes a number made of what must be the same on both (Port_NetDigest, headless.c: where the game is, and in a
   fight both fighters' health and position and the clock). A note counts once the blank is sure: every blank
   before it was run with the other player's real input (with rollback a blank is run on a guess first and noted
   again each time it is run again). The newest sure note travels in every input packet; a copy that has a sure
   note of its own for the same blank compares, and when they differ the match ends on both sides with a message
   saying so. A dozen bytes a packet and a few hundred bytes hashed twice a second. */
enum { SYNC_EVERY = 32, SYNC_SLOTS = 64, BYE_APART = 0x41504152 };
typedef struct { uint32_t tick; uint64_t value; } SyncNote;
static SyncNote sSyncMine[SYNC_SLOTS], sSyncTheirs[10][SYNC_SLOTS]; /* (by player: the host checks every other
                                                                       copy, the others the host's) */
static uint32_t sSyncOutTick, sByeWhy;
static unsigned sSyncCompared; /* (for the log: notes of the two copies compared since the last line) */
static uint64_t sSyncOutValue;
extern unsigned long long Port_NetDigest(void); /* headless.c */
static void ended(const char *why, unsigned tick);

/* The game is at the start of blank `tick` (run for the first time or again). */
static void sync_note(uint32_t tick) {
    if (tick % SYNC_EVERY == 0) {
        SyncNote *m = &sSyncMine[tick / SYNC_EVERY % SYNC_SLOTS];
        m->tick = tick;
        m->value = Port_NetDigest();
    }
}

/* Every blank up to `sure` is as it will stay (and the game is not being run again): what goes out next, and the
   other copy's notes that can be compared now. */
static void sync_check(uint32_t sure, unsigned tick) {
    uint32_t at = sure - sure % SYNC_EVERY;
    int k;

    if (at != 0 && sSyncMine[at / SYNC_EVERY % SYNC_SLOTS].tick == at) {
        sSyncOutTick = at;
        sSyncOutValue = sSyncMine[at / SYNC_EVERY % SYNC_SLOTS].value;
    }
    if (getenv("BT3_TEST_APART") != NULL && tick == (unsigned)atoi(getenv("BT3_TEST_APART"))) {
        sSyncOutValue ^= 1; /* testing: from that blank on this copy claims another game than it has, once */
    }
    for (k = 0; k < SYNC_SLOTS * sCount; k++) {
        SyncNote *o = &sSyncTheirs[k / SYNC_SLOTS][k % SYNC_SLOTS];
        if (o->tick != 0 && o->tick <= sure) {
            const SyncNote *m = &sSyncMine[k % SYNC_SLOTS];
            uint32_t was = o->tick;
            int differ = m->tick == was && m->value != o->value;
            o->tick = 0;
            sSyncCompared += m->tick == was;
            if (differ) {
                fprintf(stderr, "bt3: net: the two games differ at blank %u (this copy %016llx, player %d's %016llx)\n", (unsigned)was,
                        (unsigned long long)m->value, k / SYNC_SLOTS + 1, (unsigned long long)o->value);
                sByeWhy = BYE_APART;
                ended("The two games no longer agree (desync): the match has ended.", tick);
            }
        }
    }
}

static void gone(unsigned tick);

/* The input known from every other player: blanks below this. */
static uint32_t have_others(void) {
    uint32_t least = 0xFFFFFFFFu;
    int k;

    for (k = 0; k < sCount; k++) {
        if (k != sMe && sHave[k] < least) {
            least = sHave[k];
        }
    }
    return least;
}

static void send_pad(int who) {
    uint8_t pkt[IN_HEAD + REDUNDANT * PAD];
    uint32_t tsend = now_ms(), techo, thold;
    /* (the type word's top half says whose input it is: the host passes the others' on as they came) */
    uint32_t magic = MAGIC, type = T_INPUT | (uint32_t)who << 16, n = sHave[who] < REDUNDANT ? sHave[who] : REDUNDANT, base = sHave[who] - n, i;
    int32_t ahead = (int32_t)sTick - (int32_t)have_others(); /* how far this copy is beyond the input it has from the others */

    memcpy(pkt, &magic, 4);
    memcpy(pkt + 4, &type, 4);
    memcpy(pkt + 8, &base, 4);
    memcpy(pkt + 12, &n, 4);
    sMyAhead = ahead;
    memcpy(pkt + 16, &ahead, 4);
    memcpy(pkt + 20, &tsend, 4); /* for the ping: when this left, */
    memcpy(pkt + 32, &sSyncOutTick, 4); /* the check: the newest blank this copy is sure of, */
    memcpy(pkt + 36, &sSyncOutValue, 8); /* and what the game was at it (Port_NetDigest) */
    for (i = 0; i < n; i++) {
        memcpy(pkt + IN_HEAD + i * PAD, sIn[who][(base + i) % RING], PAD);
    }
    {   /* to the host, or from the host to each other player still there: with that one's own echo */
        int k = sMode == 1 ? 1 : 0;
        do {
            techo = sEchoTs[k];
            thold = techo != 0 ? tsend - sEchoAtMs[k] : 0;
            memcpy(pkt + 24, &techo, 4); /* the other side's last such time that arrived here, */
            memcpy(pkt + 28, &thold, 4); /* and how long ago that was */
            if (!(sLeft >> k & 1)) {
                send_to(sMode == 1 ? peer_of(k) : &sPeer, pkt, (int)(IN_HEAD + n * PAD));
            }
        } while (sMode == 1 && ++k < sCount);
    }
}

static void send_inputs(void) {
    int k;

    send_pad(sMe);
    for (k = 1; k < sCount; k++) {
        if (sLeft >> k & 1) {
            send_pad(k); /* (the host, for a player who left: their NET_GONE pads, so that every copy has them) */
        }
    }
}

/* The host: the pads of the players who left, from where their input stopped up to the host's own (kGone). */
static void left_fill(void) {
    int k;

    for (k = 1; k < sCount; k++) {
        while (sLeft >> k & 1 && sHave[k] < sHave[sMe]) {
            memcpy(sIn[k][sHave[k] % RING], kGone, PAD);
            sHave[k]++;
        }
    }
}

/* The host of more than two: player k has left (said goodbye, or went quiet); the match goes on without them. */
static void player_left(int k, const char *how) {
    if (k > 0 && k < sCount && !(sLeft >> k & 1)) {
        sLeft |= 1u << k;
        fprintf(stderr, "bt3: net: player %d %s at blank %u (their input stops at %u); the computer plays their fighter\n", k + 1, how, (unsigned)sTick,
                (unsigned)sHave[k]);
        left_fill();
    }
}

/* The lines of text that are due to go to the other player (net_chat.c: each is sent a few times). */
static void chat_send(void) {
    uint8_t pkt[16 + CHAT_TEXT];
    uint32_t head[4] = {MAGIC, T_CHAT};
    int len;

    while (sConnected && (len = PortChat_Outgoing(now_ms(), &head[2], (char *)pkt + 16)) > 0) {
        head[3] = (uint32_t)len;
        memcpy(pkt, head, 16);
        send_to_peer(pkt, 16 + len);
    }
}

/* The one who joins, from the host's answer or choices (T_HELLO_ACK, T_CONFIG: the words after the seed): its own
   player number and how many play, and player 1's options (every copy sets the two leads' sides up with them). */
static void take_place(const uint8_t *pkt, int n) {
    uint32_t w;
    int me, count;

    if (n < 36 + NAME_LEN) {
        return;
    }
    memcpy(&w, pkt + 32 + NAME_LEN, 4);
    me = (int)(w & 0xFF);
    count = (int)(w >> 8 & 0xFF);
    if (count < 2 || count > NMAX || me < 1 || me >= count) {
        return;
    }
    if (me != sMe) {
        sOpts[me] = sOpts[sMe];
        memset(sNames[sMe], 0, NAME_LEN);
    }
    sMe = me;
    sCount = count;
    if (n >= 40 + NAME_LEN && sMe != 1) {
        opts_take(1, pkt + 36 + NAME_LEN);
    }
}

/* The host's words after the seed for player k (take_place). */
static void place_words(uint32_t *after, int k) {
    after[0] = (uint32_t)k | (uint32_t)sCount << 8;
    after[1] = sOpts[1];
}

/* Takes in what has arrived. Returns 1 if anything did. */
static int receive(void) {
    uint8_t pkt[2048];
    struct sockaddr_in from;
    socklen_t fromLen = sizeof(from);
    int got = 0, n;

    queue_flush();
    chat_send();
    while ((n = net_recv(pkt, (int)sizeof(pkt), &from)) >= 8) {
        uint32_t magic, type;
        memcpy(&magic, pkt, 4);
        memcpy(&type, pkt + 4, 4);
        if (magic != MAGIC) {
            continue;
        }
        if (sConnected && (sMode == 1 ? player_at(&from) < 0 : from.sin_addr.s_addr != sPeer.sin_addr.s_addr || from.sin_port != sPeer.sin_port)) {
            continue; /* a match takes packets from its players' addresses only: nobody else can end it or put buttons in */
        }
        got = 1;
        if (type == T_PING && n >= 12) { /* setting up: sent back as it came, for the round trip time */
            uint32_t pong[3] = {MAGIC, T_PONG, 0};
            memcpy(&pong[2], pkt + 8, 4);
            send_to_peer(pong, (int)sizeof(pong)); /* (through the test line's delay, like the input) */
        } else if (type == T_PONG && n >= 12) {
            uint32_t ts;
            int rtt;
            memcpy(&ts, pkt + 8, 4);
            rtt = (int)(now_ms() - ts);
            {   /* (each player's best round trip; the line is as good as the worst of them) */
                int k = sMode == 1 ? player_at(&from) : 0, j;
                if (k >= 0 && rtt >= 0 && rtt < 5000 && (sRttOf[k] < 0 || rtt < sRttOf[k])) {
                    sRttOf[k] = rtt;
                }
                for (j = 0; j < sCount; j++) {
                    if (j != sMe && sRttOf[j] > sSetupRtt) {
                        sSetupRtt = sRttOf[j];
                    }
                }
            }
        } else if (type == T_CONFIG && sMode == 2 && n >= 16) { /* the host's choices, made after it measured the line */
            uint32_t v[2], ack[2] = {MAGIC, T_CONFIG_ACK};
            memcpy(v, pkt + 8, 8);
            sCfgDelay = (int)v[0];
            sCfgRoll = (int)v[1];
            if (n >= 24) {
                memcpy(&sRules, pkt + 20, 4);
            }
            if (n >= 24 + NAME_LEN) {
                memcpy(sNames[0], pkt + 24, NAME_LEN);
                sNames[0][NAME_LEN - 1] = '\0';
            }
            if (n >= 28 + NAME_LEN) {
                opts_take(0, pkt + 24 + NAME_LEN);
            }
            if (n >= 32 + NAME_LEN) {
                memcpy(&sSeed, pkt + 28 + NAME_LEN, 4);
            }
            take_place(pkt, n);
            if (n >= 40 + NAME_LEN + (int)sizeof(sNames)) { /* (more than two: everyone's names, for the select) */
                int k;
                memcpy(sNames, pkt + 40 + NAME_LEN, sizeof(sNames));
                for (k = 0; k < NMAX; k++) {
                    sNames[k][NAME_LEN - 1] = '\0';
                }
            }
            sCfgGot = 2;
            net_send(ack, sizeof(ack), &from);
        } else if (type == T_CONFIG_ACK && sMode == 1) {
            int k = player_at(&from);
            sCfgAcked |= k > 0 ? 1 << k : 0; /* (a bit per player who has the choices) */
        } else if (type == T_CHAT && sConnected && n >= 16) { /* (from the other player's address only: above) */
            uint32_t seq, len;
            memcpy(&seq, pkt + 8, 4);
            memcpy(&len, pkt + 12, 4);
            if (len <= CHAT_TEXT && len <= (uint32_t)(n - 16)) {
                PortChat_Heard(seq, (const char *)pkt + 16, (int)len);
            }
        } else if (type == T_BYE && sConnected) {
            uint32_t why = 0;
            if (n >= 12) {
                memcpy(&why, pkt + 8, 4);
            }
            if (sMode == 1 && sCount > 2 && why != BYE_APART) {
                player_left(player_at(&from), "left");
            } else {
                sGone = why == BYE_APART ? 2 : 1;
            }
        } else if (type == T_HELLO && sMode == 1 && !(n >= 12 && version_is(pkt + 8))) {
            static int said;
            if (!said++) {
                fprintf(stderr, "bt3: net: a copy of another version tried to join; both players need the same release\n");
            }
        } else if (type == T_HELLO && sMode == 1 && sFromLobby && n < 12 + NAME_LEN) {
            /* a greeting of the other side's lobby, still on its way or sent again because this side's answers
               were lost: answered as the lobby did, so that the other side starts its session too */
            uint32_t ack[3] = {MAGIC, T_HELLO_ACK, NET_VERSION};
            net_send(ack, sizeof(ack), &from);
        } else if (type == T_HELLO && sMode == 1) {
            uint32_t ack[10 + NAME_LEN / 4] = {MAGIC, T_HELLO_ACK, (uint32_t)sDelay, (uint32_t)sRollMax, NET_VERSION, sRules};
            int k = player_at(&from);

            if (k < 0) { /* a new player: the next number, while there is room */
                if (sJoined + 1 >= sCount) {
                    continue;
                }
                k = ++sJoined;
                if (k == 1) {
                    sPeer = from;
                } else {
                    sPeers[k] = from;
                }
            }
            ack[7 + NAME_LEN / 4] = sSeed;
            memcpy(&ack[6], sMyName, NAME_LEN);
            ack[6 + NAME_LEN / 4] = sOpts[0];
            if (n >= 12 + NAME_LEN) { /* the other player's name came with the greeting */
                memcpy(sNames[k], pkt + 12, NAME_LEN);
                sNames[k][NAME_LEN - 1] = '\0';
            }
            if (n >= 16 + NAME_LEN) {
                opts_take(k, pkt + 12 + NAME_LEN);
            }
            place_words(&ack[8 + NAME_LEN / 4], k);
            {   /* (does the other copy know the 120% pace? if not, the match is at the game's own) */
                uint32_t cap = 0;
                if (n >= 20 + NAME_LEN) {
                    memcpy(&cap, pkt + 16 + NAME_LEN, 4);
                }
                if ((cap & 0xFFFF0000u) != CAP_MARK) {
                    cap = 0;
                }
                if (!(cap & 2)) { /* does not know the number: 120% if it knows that and it is that, else 100% */
                    sRules &= ~(0xFFu << RULE_SPEED_SHIFT);
                }
                if (!(cap & 1)) {
                    sRules &= ~RULE_FAST;
                }
                ack[5] = sRules;
            }
            memcpy(sNames[0], sMyName, NAME_LEN);
            /* (the host learned the player's address from its greeting, above) the match begins with everyone */
            sConnected = sJoined + 1 >= sCount;
            net_send(ack, sizeof(ack), &from);
        } else if (type == T_HELLO_ACK && sMode == 2 && sFromLobby && n < 20) {
            /* an answer to the lobby's greeting, still in the socket the session took over: nothing to do */
        } else if (type == T_HELLO_ACK && sMode == 2) {
            if (!(n >= 20 && version_is(pkt + 16))) {
                sVersionBad = 1;
            } else if (!sConnected) {
                uint32_t v[2];
                memcpy(v, pkt + 8, 8);
                sCfgDelay = (int)v[0];
                sCfgRoll = (int)v[1];
                if (n >= 24) {
                    memcpy(&sRules, pkt + 20, 4);
                }
                if (n >= 24 + NAME_LEN) {
                    memcpy(sNames[0], pkt + 24, NAME_LEN);
                    sNames[0][NAME_LEN - 1] = '\0';
                }
                if (n >= 28 + NAME_LEN) {
                    opts_take(0, pkt + 24 + NAME_LEN);
                }
                if (n >= 32 + NAME_LEN) {
                    memcpy(&sSeed, pkt + 28 + NAME_LEN, 4);
                }
                take_place(pkt, n);
                memcpy(sNames[sMe], sMyName, NAME_LEN);
                sCfgGot = 1;
                sConnected = 1;
            }
        } else if ((type & 0xFFFFu) == T_INPUT && n >= IN_HEAD && (sConnected || sMode == 1)) { /* (the joining side: not before the host's answer) */
            uint32_t base, count, i, other = type >> 16; /* (whose input: the type word's top half) */
            int own;                                     /* the sender's own packet, not one the host passed on */

            if (sMode == 1) {
                int k = player_at(&from);
                if (k < 0 && sCount == 2 && !sConnected) { /* (two players: the input says the greeting got through) */
                    sPeer = from;
                    sJoined = 1;
                    k = 1;
                }
                if (k < 0 || other != (uint32_t)k || sLeft >> k & 1) {
                    continue; /* a player puts in its own input only (and not after it left: the host plays it) */
                }
                own = 1;
            } else {
                if (other >= (uint32_t)sCount || other == (uint32_t)sMe) {
                    continue;
                }
                own = other == 0;
            }
            memcpy(&base, pkt + 8, 4);
            memcpy(&count, pkt + 12, 4);
            if (own) {
                uint32_t at;
                memcpy(&sTheirAhead, pkt + 16, 4);
                memcpy(&at, pkt + 32, 4);
                if (at != 0) { /* the other copy's check value for that blank: compared when this copy is sure of it too */
                    SyncNote *o = &sSyncTheirs[other][at / SYNC_EVERY % SYNC_SLOTS];
                    o->tick = at;
                    memcpy(&o->value, pkt + 36, 8);
                }
            }
            if (sMode == 2 || sCount == 2) {
                sConnected = 1; /* (an input packet says the greeting got through) */
            }
            if (sMode == 1 && sCount > 2) { /* the host passes it on to every other player, as it came */
                int k;
                for (k = 1; k < sCount; k++) {
                    if ((uint32_t)k != other) {
                        send_to(peer_of(k), pkt, n);
                    }
                }
            }
            if (own) {
                uint32_t ts, echo, hold, now = now_ms();
                memcpy(&ts, pkt + 20, 4);
                memcpy(&echo, pkt + 24, 4);
                memcpy(&hold, pkt + 28, 4);
                sEchoTs[other] = ts;
                sEchoAtMs[other] = now;
                if (echo != 0 && now - echo - hold < 5000u) { /* the round trip: there and back, less their holding time */
                    /* The smallest of the last 64: a packet waits in the socket until the next blank looks, on both
                       sides, which adds up to two blanks to a single measurement and nothing to the best one. */
                    static int ring[64], at, filled;
                    int rtt = (int)(now - echo - hold), k, best;
                    ring[at++ & 63] = rtt;
                    filled = filled < 64 ? filled + 1 : 64;
                    best = ring[0];
                    for (k = 1; k < filled; k++) {
                        best = ring[k] < best ? ring[k] : best;
                    }
                    sPingMs = best;
                }
            }
            for (i = 0; i < count && IN_HEAD + (i + 1) * PAD <= (uint32_t)n; i++) {
                if (base + i >= sHave[other]) {
                    if (base + i > sHave[other]) {
                        break; /* a gap: wait for a packet that covers it */
                    }
                    memcpy(sIn[other][(base + i) % RING], pkt + IN_HEAD + i * PAD, PAD);
                    sHave[other] = base + i + 1;
                }
            }
        }
    }
    return got;
}

/* Testing: this copy behaves like a weak machine (real time only; nothing of the game's state may follow from it).
     BT3_TEST_SLOW_READ=<ms>    every file read of the game takes that long more (what = 0)
     BT3_TEST_SLOW_START=<ms>   that long between the connection and the session's first blank (what = 1)
     BT3_TEST_SLOW_BLANK=<ms>   every blank of a match takes that long more (what = 2) */
void Port_TestSlow(int what, int amount) {
    static int ms[3] = {-1, -1, -1};
    (void)amount;
    if (ms[0] < 0) {
        ms[0] = getenv("BT3_TEST_SLOW_READ") != NULL ? atoi(getenv("BT3_TEST_SLOW_READ")) : 0;
        ms[1] = getenv("BT3_TEST_SLOW_START") != NULL ? atoi(getenv("BT3_TEST_SLOW_START")) : 0;
        ms[2] = getenv("BT3_TEST_SLOW_BLANK") != NULL ? atoi(getenv("BT3_TEST_SLOW_BLANK")) : 0;
    }
    if (what >= 0 && what < 3 && ms[what] > 0) {
        SDL_Delay((Uint32)ms[what]);
    }
}

static int sSession; /* a session started from Dragon Net Battle (below), not a plain connected run */
static int sArrived; /* its start-up has reached the character select */
static uint32_t sStartedMs; /* when the two were connected (for the log) */

/* Connects: role 1 hosts on `port`, role 2 joins `join` ("address:port"). Waits until the other side is there. */
static void net_start(int role, const char *host, const char *join) {
    struct sockaddr_in local;
    uint64_t t0, last = 0;
    uint32_t i;

    sMode = role;
    sConnected = 0;
    sTick = 0;
    memset(sIn, 0, sizeof(sIn));
    memset(sHave, 0, sizeof(sHave)); /* (a second session in one run starts as clean as the first) */
    PortChat_Reset();
    sWaitNs = sWaits = 0;
    sFromLobby = sHandSock >= 0;
    if (sFromLobby) {
        sSock = sHandSock;
        sHandSock = -1;
    } else {
#ifdef _WIN32
        WSADATA w;
        u_long on = 1;
        WSAStartup(MAKEWORD(2, 2), &w);
        sSock = (int)socket(AF_INET, SOCK_DGRAM, 0);
        ioctlsocket(sSock, FIONBIO, &on);
#else
        sSock = socket(AF_INET, SOCK_DGRAM, 0);
        fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL, 0) | O_NONBLOCK);
#endif
    }
    sMe = sMode - 1;
    /* BT3_NET_PLAYERS=<n> on the host (tests, and across the restart into the session): that many play, 2..NMAX.
       The others learn theirs from its answer (take_place). */
    sCount = sMode == 1 && getenv("BT3_NET_PLAYERS") != NULL ? atoi(getenv("BT3_NET_PLAYERS")) : 2;
    if (sCount < 2 || sCount > NMAX) {
        sCount = 2;
    }
    sJoined = 0;
    memset(sPeers, 0, sizeof(sPeers));
    {
        int k;
        for (k = 0; k < NMAX; k++) {
            sRttOf[k] = -1;
        }
    }
    if (getenv("BT3_NET_LOCALPORT") != NULL && sCodeLocalPort == 0) { /* (across the restart into the session) */
        sCodeLocalPort = atoi(getenv("BT3_NET_LOCALPORT"));
    }
    if (getenv("BT3_NET_PUNCH") != NULL && !sCodePunchSet) {
        char text[64], *colon;
        snprintf(text, sizeof(text), "%s", getenv("BT3_NET_PUNCH"));
        colon = strrchr(text, ':');
        if (colon != NULL) {
            *colon = '\0';
            memset(&sCodePunch, 0, sizeof(sCodePunch));
            sCodePunch.sin_family = AF_INET;
            sCodePunch.sin_port = htons((uint16_t)atoi(colon + 1));
            sCodePunchSet = inet_pton(AF_INET, text, &sCodePunch.sin_addr) == 1;
        }
    }
    if (getenv("BT3_NET_NAME") != NULL) { /* (across the restart into the session) */
        Port_NetNameSet(getenv("BT3_NET_NAME"));
    }
    memset(sNames, 0, sizeof(sNames));
    memcpy(sNames[sMe], sMyName, NAME_LEN);
    memset(sOpts, 0, sizeof(sOpts));
    sOpts[sMe] = own_options();
    if (getenv("BT3_NET_RULES") != NULL) { /* (the host's, across the restart into the session, or for a test) */
        sRules = (uint32_t)strtoul(getenv("BT3_NET_RULES"), NULL, 0);
    }
    if (getenv("BT3_NET_SPEED") != NULL) { /* testing: 120 or 100 */
        Port_NetSpeedSet(atoi(getenv("BT3_NET_SPEED")));
    }
    sAutoDelay = getenv("BT3_NET_DELAY") != NULL ? strcmp(getenv("BT3_NET_DELAY"), "auto") == 0 : sOptAuto;
    sDelay = getenv("BT3_NET_DELAY") != NULL && !sAutoDelay ? atoi(getenv("BT3_NET_DELAY")) : sOptDelay >= 0 ? sOptDelay : 2;
    sSetupRtt = -1;
    sCfgAcked = 0;
    if (sDelay < 0) { sDelay = 0; }
    if (sDelay > 30) { sDelay = 30; }
    sCfgGot = 0;
    sGone = 0;
    sLeft = 0;
    sVersionBad = 0;
    memset(sEchoTs, 0, sizeof(sEchoTs));
    sPingMs = -1;
    sSecRoll = sSecRollTicks = sSecWaits = sSecWaitMs = sSecTicks = 0;
    memset(sShown, 0, sizeof(sShown));
    sLoss = getenv("BT3_NET_LOSS") != NULL ? atoi(getenv("BT3_NET_LOSS")) : 0;
    sLagMs = getenv("BT3_NET_LAG") != NULL ? atoi(getenv("BT3_NET_LAG")) : 0;
    sLatencyMs = getenv("BT3_NET_LATENCY") != NULL ? atoi(getenv("BT3_NET_LATENCY")) : 0;
    /* BT3_NET_ROLLBACK=<n>: the game does not wait for the other player's input; it goes on with a guess (what
       they held last) for up to n blanks and, when the real input differs, goes back and runs those blanks again
       (roll_tick). Needs the roll ring of gs/state.c (the 64-bit programs); 0 or unset: wait, as before. */
    sRollMax = !Port_RollCan() ? 0 : getenv("BT3_NET_ROLLBACK") != NULL ? atoi(getenv("BT3_NET_ROLLBACK")) : sOptRoll >= 0 ? sOptRoll : 0;
    if (sRollMax < 0) { sRollMax = 0; }
    if (sRollMax > 30) { sRollMax = 30; }
    sChecked = sLive = 0;
    sResim = sRollBegun = 0;
    memset(sSyncMine, 0, sizeof(sSyncMine));
    memset(sSyncTheirs, 0, sizeof(sSyncTheirs));
    sSyncOutTick = 0;
    sSyncOutValue = 0;
    sByeWhy = 0;
    sTheirAhead = 0;
    sBalance = 0;
    sRollbacks = sRollTicks = sStalls = 0;
    if (getenv("BT3_NET_SCRIPT") != NULL) {
        sScript = fopen(getenv("BT3_NET_SCRIPT"), "rb");
    }
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    memset(&sPeer, 0, sizeof(sPeer));
    sPeer.sin_family = AF_INET;
    sSeed = 0;
    if (sMode == 1) {
        seed_draw();
        local.sin_port = htons((uint16_t)atoi(host));
        if (!sFromLobby && bind(sSock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            fprintf(stderr, "bt3: net: cannot use port %s\n", host);
            exit(2);
        }
        fprintf(stderr, "bt3: net: hosting on port %s (player 1), waiting for the other player...\n", host);
    } else {
        char addr[128];
        char *colon;
        snprintf(addr, sizeof(addr), "%s", join);
        colon = strrchr(addr, ':');
        if (colon == NULL) {
            fprintf(stderr, "bt3: net: BT3_NET_JOIN wants <address>:<port>\n");
            exit(2);
        }
        *colon = '\0';
        sPeer.sin_port = htons((uint16_t)atoi(colon + 1));
        if (inet_pton(AF_INET, addr, &sPeer.sin_addr) != 1) {
            struct hostent *he = gethostbyname(addr);
            if (he == NULL) {
                fprintf(stderr, "bt3: net: unknown address %s\n", addr);
                exit(2);
            }
            memcpy(&sPeer.sin_addr, he->h_addr_list[0], sizeof(sPeer.sin_addr));
        }
        if (sCodeLocalPort != 0 && !sFromLobby) {
            /* a match found with a room code: from the port the lobby used, so that the routers on the way keep
               taking this for the same conversation */
            local.sin_port = htons((uint16_t)sCodeLocalPort);
            bind(sSock, (struct sockaddr *)&local, sizeof(local));
        }
        /* The address is only printed when the player typed it. One that came from the matchmaking service (a
           room code, a quick match) is the other player's, and logs get pasted into bug reports. */
        if (sCodeLocalPort != 0) {
            fprintf(stderr, "bt3: net: joining the host found by the matchmaking service (player 2)...\n");
        } else {
            fprintf(stderr, "bt3: net: joining %s (player 2)...\n", join);
        }
    }
    t0 = SDL_GetTicksNS();
    while (!sConnected) {
        uint64_t now = SDL_GetTicksNS();
        if (sMode == 1 && sCodePunchSet && now - last > 100000000ull) {
            uint32_t punch[2] = {MAGIC, T_PUNCH}; /* (room codes: keeps this side's router open for the one who joins) */
            net_send(punch, sizeof(punch), &sCodePunch);
            last = now;
        }
        if (sMode == 2 && now - last > 100000000ull) {
            uint32_t hello[5 + NAME_LEN / 4] = {MAGIC, T_HELLO, NET_VERSION};
            memcpy(&hello[3], sMyName, NAME_LEN);
            hello[3 + NAME_LEN / 4] = sOpts[1];
            hello[4 + NAME_LEN / 4] = CAP_MARK | 3;
            net_send(hello, sizeof(hello), &sPeer);
            last = now;
        }
        receive();
        if (sVersionBad) {
            fprintf(stderr, "bt3: net: the other player's game is another version; both need the same release\n");
            Port_UiNotice("The other player's game is a different version.");
            if (sSession) {
                Port_NetLeave();
            }
            exit(2);
        }
        SDL_Delay(2);
        if (now - t0 > 120000000000ull) {
            fprintf(stderr, "bt3: net: nobody answered in two minutes\n");
            exit(2);
        }
    }
    /* Setting up. The host measures the round trip (a dozen pings, the smallest counts), chooses the input delay
       from it if that is automatic, and tells the other side its choices until that side has them; the other
       side answers the pings and waits for the choices. */
    if (sMode == 1) {
        uint64_t start = SDL_GetTicksNS(), lastPing = 0, lastCfg = 0;
        int pings = 0;
        while (SDL_GetTicksNS() - start < 400000000ull) {
            uint64_t now = SDL_GetTicksNS();
            if (pings < 12 && now - lastPing >= 25000000ull) {
                uint32_t ping[3] = {MAGIC, T_PING, now_ms()};
                send_to_peer(ping, (int)sizeof(ping));
                lastPing = now;
                pings++;
            }
            receive();
            SDL_Delay(1);
        }
        if (sAutoDelay) {
            sDelay = auto_delay(sSetupRtt);
        }
        fprintf(stderr, "bt3: net: round trip %d ms%s\n", sSetupRtt, sAutoDelay ? " (the input delay is chosen from it)" : "");
        start = SDL_GetTicksNS();
        while (sCfgAcked != (1 << sCount) - 2 && SDL_GetTicksNS() - start < 3000000000ull) { /* (every other player) */
            uint64_t now = SDL_GetTicksNS();
            if (now - lastCfg >= 50000000ull) {
                int k;
                for (k = 1; k < sCount; k++) {
                    uint32_t cfg[10 + NAME_LEN / 4 + sizeof(sNames) / 4] = {MAGIC, T_CONFIG, (uint32_t)sDelay, (uint32_t)sRollMax, NET_VERSION, sRules};
                    if (sCfgAcked & 1 << k) {
                        continue;
                    }
                    cfg[7 + NAME_LEN / 4] = sSeed;
                    memcpy(&cfg[6], sMyName, NAME_LEN);
                    cfg[6 + NAME_LEN / 4] = sOpts[0];
                    place_words(&cfg[8 + NAME_LEN / 4], k);
                    memcpy(&cfg[10 + NAME_LEN / 4], sNames, sizeof(sNames)); /* (more than two: everyone's names) */
                    net_send(cfg, sCount > 2 ? (int)sizeof(cfg) : (int)(40 + NAME_LEN), peer_of(k));
                }
                lastCfg = now;
            }
            receive();
            SDL_Delay(1);
        }
    } else {
        uint64_t start = SDL_GetTicksNS();
        while (sCfgGot != 2 && SDL_GetTicksNS() - start < 5000000000ull) {
            receive();
            SDL_Delay(1);
        }
    }
    if (sMode == 2 && sCfgGot) { /* the host's choices, unless this side was started with its own */
        if ((getenv("BT3_NET_DELAY") == NULL || sAutoDelay) && sCfgDelay >= 0 && sCfgDelay <= 30) {
            sDelay = sCfgDelay;
        }
        if (getenv("BT3_NET_ROLLBACK") == NULL && sOptRoll < 0 && sCfgRoll >= 0 && sCfgRoll <= 30) {
            sRollMax = Port_RollCan() ? sCfgRoll : 0;
        }
    }
    /* the blanks before any input can have arrived (0 .. delay) are idle for every player */
    {
        int k;
        for (k = 0; k < sCount; k++) {
            for (i = 0; i <= (uint32_t)sDelay; i++) {
                memcpy(sIn[k][i], kIdle, PAD);
            }
            sHave[k] = (uint32_t)sDelay + 1;
        }
    }
    if (sCount > 2) {
        fprintf(stderr, "bt3: net: %d players; this copy is player %d\n", sCount, sMe + 1);
    }
    fprintf(stderr, "bt3: net: connected%s; input delay %d blanks, %s\n", sFromLobby && Port_LobbyRelayed() ? " through the relay" : sFromLobby ? " directly" : "", sDelay,
            sRollMax > 0 ? "rollback" : "waiting for each other");
    fprintf(stderr, "bt3: net: game speed %d%%\n", rules_speed());
    if (sOpts[0] != 0 && sOpts[1] != 0) {
        fprintf(stderr, "bt3: net: each player's own options: player 1 camera %u shake %u, player 2 camera %u shake %u\n", (unsigned)(sOpts[0] & 0xFF),
                (unsigned)(sOpts[0] >> 8 & 0xFF), (unsigned)(sOpts[1] & 0xFF), (unsigned)(sOpts[1] >> 8 & 0xFF));
    } else {
        fprintf(stderr, "bt3: net: the other copy does not send its player's options (0.1.25 or earlier): both sides use the defaults\n");
    }
    if (sRollMax > 0) {
        fprintf(stderr, "bt3: net: rollback of up to %d blanks\n", sRollMax);
    }
    sStartedMs = now_ms();
    Port_TestSlow(1, 0);
}

int Port_NetActive(void);

/* The voice language the player chose in the game's options, read from their own save (bit 0 of the flags word at
   0x1608: set = the first choice of the menu, which is what a new save has; clear = the second voice set). An
   online match starts from an empty save on both sides, so both compute with the default; this choice is kept
   aside for what this copy plays (BT3_NET_VOICE=alt; snd_adx.c, snd_se.c). 1 = the second set. */
static int own_voice_alt(void) {
    const char *root = getenv("BT3_SAVES") != NULL ? getenv("BT3_SAVES") : "saves";
    char path[600];
    unsigned char w[4];
    FILE *fp;
    int alt = 0;

    snprintf(path, sizeof(path), "%s/card1/%s/%s", root, NET_SAVE_DIR, NET_SAVE_DIR);
    fp = fopen(path, "rb");
    if (fp != NULL) {
        if (fseek(fp, 0x1608, SEEK_SET) == 0 && fread(w, 1, 4, fp) == 4) {
            alt = !(w[0] & 1);
        }
        fclose(fp);
    }
    return alt;
}

/* Whether this copy plays the second voice set in the online match it is in (the player's own choice, above). */
static int sVoiceAlt = -1; /* 1 the second set, 0 the default; -1 not decided (then BT3_NET_VOICE says, once) */

int Port_NetVoiceAlt(void) {
    /* (Not asked of the environment every time: the port's getenv remembers its first answer at each place it is
       called from (gs_internal.h), and a match that begins without the program starting again sets the variable
       after that first look. Found on Windows, 2026-10-09: the choice was never seen. The match's start sets
       sVoiceAlt itself; the variable is for the program that starts again as the session, and for tests.) */
    if (sVoiceAlt < 0) {
        const char *v = getenv("BT3_NET_VOICE");
        sVoiceAlt = v != NULL && strcmp(v, "alt") == 0;
    }
    return Port_NetActive() && sVoiceAlt == 1;
}

/* The players' names (the lobby window's "Player name"), for the line under each side's gauges: this copy's own
   goes out with its greeting (T_HELLO, the one who joins) or its answer and choices (T_HELLO_ACK, T_CONFIG, the
   host), 20 bytes after the words those packets had. */

/* The name of player `player` (0 the host) in the match this copy is in ("" if it is not known). */
const char *Port_NetName(int player) {
    return Port_NetActive() && player >= 0 && player < sCount ? sNames[player] : "";
}

/* Whether this copy shows the two sides of the fight's HUD in each other's places: the player who joined is side 1,
   and with this their own gauges are on the left, as the host's are for the host. Setting net_hud_own (on by
   default); BT3_HUD_SWAP=1 the same without a connection, for looking at it. Only this copy's picture changes. */
int Port_HudSwap(void) {
    static int forced = -1, pref = -1;
    extern int Port_Setting(const char *name, int def);
    if (forced < 0) {
        forced = getenv("BT3_HUD_SWAP") != NULL ? atoi(getenv("BT3_HUD_SWAP")) != 0 : 0;
        pref = Port_Setting("net_hud_own", 1) != 0;
    }
    if (forced || !pref || !Port_NetActive()) {
        return forced;
    }
    {   /* (the player's own panel is side 1's: the one who joined, or a fighter of side 1's team beyond the leads) */
        extern int BattleSide_GetTeam(int side); /* src/battle/battle_load.c */
        return sMe == 1 || (sMe >= 2 && BattleSide_GetTeam(sMe) == 1);
    }
}

/* Which player this copy plays in an online match (0 the host, 1 the one who joined). */
int Port_NetMe(void) {
    return sMe;
}

int Port_NetActive(void) {
    if (sMode < 0) { /* first asked: connected from the start if the environment says so */
        const char *host = getenv("BT3_NET_HOST"), *join = getenv("BT3_NET_JOIN");
        sMode = 0;
        if (host != NULL || join != NULL) {
            sSession = getenv("BT3_NET_SESSION") != NULL;
            net_start(host != NULL ? 1 : 2, host, join);
        }
    }
    return sMode > 0;
}

/* A new vertical blank begins (Port_VBlank): this player's input made now takes effect `delay` blanks on; then
   wait until the other player's input for THIS blank is here. */
/* The local player's input of this blank, entered for the blank `delay` later (once per blank). */
static void sample_local(unsigned tick) {
    uint8_t mine[PAD];
    uint32_t at = (uint32_t)tick + (uint32_t)sDelay;

    if (at < sHave[sMe]) {
        return;
    }
    memcpy(mine, kIdle, PAD);
    if (sScript != NULL) {
        uint8_t all[NMAX * PAD]; /* (a table holds every player's pad per blank: sCount of them) */
        size_t row = (size_t)sCount * PAD;
        if (fseek(sScript, (long)tick * (long)row, SEEK_SET) == 0 && fread(all, 1, row, sScript) == row) {
            memcpy(mine, all + sMe * PAD, PAD);
        }
    } else if (!Port_PadRead(0, mine)) {
        memcpy(mine, kIdle, PAD);
    }
    memcpy(sIn[sMe][at % RING], mine, PAD);
    sHave[sMe] = at + 1;
}

#define QUIET_NS 6000000000ull /* this long without the input the game is waiting for: the other player is gone */

static void ended(const char *why, unsigned tick) {
    fprintf(stderr, "bt3: net: %s (blank %u)\n", why, tick);
    Port_UiNotice(why);
    if (sSession) {
        Port_NetLeave(); /* back to the game as it normally is */
    }
    exit(2);
}

/* The other copy said goodbye. */
static void gone(unsigned tick) {
    ended(sGone == 2 ? "The two games no longer agree (desync): the match has ended." : "The other player left the match.", tick);
}

/* Waited QUIET_FOR for input. The host of more than two goes on without whoever it waits for (and decides that
   before the others would give up waiting for them, QUIET_NS); otherwise the match is over. */
#define QUIET_FOR (sMode == 1 && sCount > 2 ? QUIET_NS / 2 : QUIET_NS)
static void gone_quiet(unsigned tick) {
    uint32_t least = have_others();
    int k;

    if (sMode == 1 && sCount > 2) {
        for (k = 1; k < sCount; k++) {
            if (sHave[k] == least) {
                player_left(k, "went quiet");
            }
        }
        return;
    }
    ended("The connection to the other player was lost.", tick);
}

/*
 * Rollback. Each blank's state is saved before the blank reads its pads (Port_RollSave). The other player's pad
 * for a blank whose input has not arrived reads as their last known input, and what each blank was given is
 * remembered (sUsed). When input arrives that differs from what a past blank was given, the game goes back to
 * that blank's save and runs up to the present again, without picture or sound (gPortResim), now with the right
 * input. It never runs more than sRollMax blanks ahead of the other player's last known input: there it waits.
 */
static void roll_tick(unsigned tick) {
    uint32_t t, limit, sure, from;
    uint64_t t0, last;
    int p;

    if (!sRollBegun) {
        sRollBegun = 1;
        sChecked = tick; /* nothing was given to an earlier blank */
    }
    sync_note(tick); /* (the game as this blank finds it, whether the blank is new or run again) */
    if (sResim && tick >= sLive) {
        sResim = 0; /* caught up: this blank is the present again */
    }
    if (!sResim) {
        Port_TestSlow(2, 0);
        sample_local(tick);
        left_fill();
        send_inputs();
        receive();
        if (sGone) {
            gone(tick);
        }
        t0 = last = SDL_GetTicksNS();
        if (tick >= have_others() + (uint32_t)sRollMax) {
            sStalls++;
            sSecWaits++;
        }
        while (tick >= have_others() + (uint32_t)sRollMax) {
            uint64_t now = SDL_GetTicksNS();
            if (now - last > 4000000ull) {
                send_inputs();
                last = now;
            }
            if (!receive()) {
                SDL_DelayNS(200000);
            }
            if (sGone) {
                gone(tick);
            }
            if (now - t0 > QUIET_FOR) {
                gone_quiet(tick);
                t0 = now;
            }
        }
        {   /* (see sTheirAhead) */
            int diff = sMode == 1 && sCount > 2 ? 0 : (int)sMyAhead - (int)sTheirAhead; /* (both taken at the same point of a
                blank: when sending. The others keep to the host; the host of more than two keeps its own pace) */
            if (getenv("BT3_NET_SKEW") != NULL && tick == 1500) { /* testing: this copy falls behind by that many ms, once */
                gPortPaceShiftNs = (unsigned)atoi(getenv("BT3_NET_SKEW")) * 1000000u;
            }
            sBalance += (diff * 16 - sBalance) / 16;
            if (sBalance >= 24 && getenv("BT3_NET_NOBALANCE") == NULL) { /* a blank and a half apart or more: a millisecond later */
                gPortPaceShiftNs += 1000000;
                sSlowed++;
            }
        }
        sSecWaitMs += (unsigned)((SDL_GetTicksNS() - t0) / 1000000ull);
        if (++sSecTicks >= 60) { /* a second of blanks: what the meter shows next */
            sShown[1] = (int)sSecRoll;
            sShown[2] = sSecRoll != 0 ? (int)(sSecRollTicks * 10 / sSecRoll) : 0;
            sShown[3] = (int)sSecWaits;
            sShown[4] = (int)sSecWaitMs;
            sSecRoll = sSecRollTicks = sSecWaits = sSecWaitMs = sSecTicks = 0;
        }
        /* each other player: the first blank that was given something else than what they really pressed (from),
           and how far every blank was given their real input (sure) */
        from = 0xFFFFFFFFu;
        sure = 0xFFFFFFFFu;
        for (p = 0; p < sCount; p++) {
            if (p == sMe) {
                continue;
            }
            limit = sHave[p] < tick ? sHave[p] : tick;
            for (t = sChecked; t < limit && memcmp(sUsed[p][t % RING], sIn[p][t % RING], PAD) == 0; t++) {
            }
            if (t < limit && t < from) {
                from = t;
            }
            if (t < sure) {
                sure = t;
            }
        }
        sChecked = sure; /* (not past `from`: another player's blanks before it may still be guesses) */
        t = from != 0xFFFFFFFFu ? from : sure;
        if (from != 0xFFFFFFFFu) { /* blank t was given something else than what someone really pressed */
            sLive = tick;
            sResim = 1;
            sRollbacks++;
            sRollTicks += tick - t;
            sSecRoll++;
            sSecRollTicks += tick - t;
            gPortResim = 1;
            Port_RollBack((int)(tick - t)); /* does not return: goes on after blank t's Port_RollSave below */
        }
        sync_check(sChecked, tick); /* (every blank below sChecked was run with their real input) */
        if (tick % 600 == 0 && getenv("BT3_GS_VERBOSE") != NULL) {
            fprintf(stderr, "net: blank %u: %u rollbacks (%.1f blanks each), %u waits in the last 600 blanks; ahead %d, they %d, slowed %u ms; %u checks against the other copy\n", tick, sRollbacks,
                    sRollbacks != 0 ? (double)sRollTicks / sRollbacks : 0.0, sStalls, (int)sMyAhead, (int)sTheirAhead, sSlowed, sSyncCompared);
            sRollbacks = sRollTicks = sStalls = sSlowed = sSyncCompared = 0;
        }
    }
    if (Port_RollSave() != 0) {
        sTick = tick; /* back at this blank, to run from here again */
    }
    gPortResim = sResim || Port_NetWarp();
    for (p = 0; p < sCount; p++) {
        if (p != sMe) {
            memcpy(sUsed[p][tick % RING], sIn[p][(tick < sHave[p] ? tick : sHave[p] - 1) % RING], PAD);
        }
    }
}

void Port_NetBeginTick(unsigned tick) {
    uint64_t t0, last;

    if (!Port_NetActive()) {
        return;
    }
    sTick = tick;
    if (sMode == 2 && getenv("BT3_TEST_LEAVE") != NULL && tick == (unsigned)atoi(getenv("BT3_TEST_LEAVE"))) {
        uint32_t bye[3] = {MAGIC, T_BYE, 0}; /* testing: this copy says goodbye at that blank and stops */
        net_send(bye, sizeof(bye), &sPeer);
        exit(0);
    }
    if (sRollMax > 0) {
        roll_tick(tick);
        return;
    }
    sample_local(tick);
    left_fill();
    if (sLagMs != 0) {
        SDL_Delay((Uint32)sLagMs); /* (a crude stand-in for a slow line: everything this copy sends is late) */
    }
    send_inputs();
    receive();
    t0 = last = SDL_GetTicksNS();
    while (have_others() <= tick) {
        uint64_t now = SDL_GetTicksNS();
        if (now - last > 4000000ull) {
            send_inputs(); /* the other side may be waiting for a packet that was lost */
            last = now;
        }
        if (!receive()) {
            SDL_DelayNS(200000);
        }
        if (sGone) {
            gone(tick);
        }
        if (now - t0 > QUIET_FOR) {
            gone_quiet(tick);
            t0 = now;
        }
    }
    sWaitNs += SDL_GetTicksNS() - t0;
    sWaits++;
    sync_note(tick); /* (nothing is guessed here: every blank is sure when it begins) */
    sync_check(tick, tick);
    {   /* (the meter, when the two copies wait for each other every blank: time waited, and blanks that waited
           more than a millisecond) */
        unsigned ms = (unsigned)((SDL_GetTicksNS() - t0) / 1000000ull);
        sSecWaitMs += ms;
        sSecWaits += ms >= 1;
        if (++sSecTicks >= 60) {
            sShown[1] = sShown[2] = 0;
            sShown[3] = (int)sSecWaits;
            sShown[4] = (int)sSecWaitMs;
            sSecWaits = sSecWaitMs = sSecTicks = 0;
        }
    }
    if (tick % 600 == 0 && getenv("BT3_GS_VERBOSE") != NULL) {
        fprintf(stderr, "net: blank %u: waited for the other player %.2f ms per blank on average; %u checks against the other copy\n", tick, (double)sWaitNs / (double)sWaits / 1e6,
                sSyncCompared);
        sWaitNs = sWaits = 0;
        sSyncCompared = 0;
    }
}

/* What the game reads for a pad during this blank. */
void Port_NetInput(int player, unsigned char *data) {
    if (sRollMax > 0 && player >= 0 && player < sCount && player != sMe) {
        memcpy(data, sUsed[player][sTick % RING], PAD); /* their real input, or the guess (roll_tick) */
        return;
    }
    memcpy(data, player >= 0 && player < sCount && sTick < sHave[player] ? sIn[player][sTick % RING] : kIdle, PAD);
}

/* The game: the computer now plays the fighter of pad `pad`, whose player left (kGone; for the log). */
void Port_NetCpuTakes(int pad) {
    if (!sResim) {
        fprintf(stderr, "bt3: net: blank %u: the computer plays the fighter of pad %d\n", (unsigned)sTick, pad + 1);
    }
}

/* How many play in the match this copy is in (2 when it is in none). */
int Port_NetPlayers(void) {
    return Port_NetActive() ? sCount : 2;
}

int Port_NetResim(void) {
    return sResim;
}

/* Which player's view this copy shows full screen in a two-player battle: the local player's when online; -1
   otherwise (the two views side by side, as always). BT3_VIEW=0|1: the same without a connection, for looking at
   it and for comparing the game's state between the three ways of showing a battle. */
static int sViewForced = -2;
int Port_NetView(void) {
    if (sViewForced == -2) {
        const char *e = getenv("BT3_VIEW");
        sViewForced = e != NULL ? atoi(e) : -1;
    }
    if (sViewForced >= 0) {
        return sViewForced & 1;
    }
    return Port_NetActive() ? (sMe < 2 ? sMe : 0) : -1; /* (a third player on: view 0, which follows their fighter) */
}

/* A match of more than two: the fighter of this copy's player when it is beyond the two leads. This copy's view 0
   then follows that fighter's camera (btl_cam.c, BtlCam_UpdateView): each player has a screen of their own, as with
   two. -1 otherwise, and when a view is forced (BT3_VIEW: tests that compare everything between the copies). */
int Port_NetViewFighter(void) {
    return Port_NetView() >= 0 && sViewForced < 0 && sMe >= 2 ? sMe : -1;
}

/* This copy shows its own player's screen (online, no view forced): its camera, and the HUD's panels of its own
   fighter and of the one that fighter is set on (multi_fighter.c, PortMulti_HudFighter). */
int Port_NetOwnScreen(void) {
    return Port_NetView() >= 0 && sViewForced < 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * The session started from the game's "Dragon Net Battle" entry.
 *
 * Two copies can only stay in step if they start from the same state, and two players coming from their own menus
 * with their own saves are not in the same state. So a session is a fresh start of the program on both sides:
 *   1. the lobby (the window behind the menu entry): one side hosts, the other joins; the two find each other
 *      (Port_Lobby*, a greeting and its answer);
 *   2. each side then starts the program again with BT3_NET_SESSION=1 and its role, an empty save folder of the
 *      session's own (net_session/), and connects again as above. The game boots without picture and sound and
 *      not held to real time, straight into the versus mode's menu (headless.c), with everything unlocked by the
 *      game's own Save_UnlockAll, so both sides have the same roster;
 *   3. from there it is the game's own two-player versus mode, the host's pad also driving the menus;
 *   4. leaving the versus mode for the main menu ends the session: each side starts the program once more, as
 *      it normally is, with its own save.
 * ------------------------------------------------------------------------------------------------------------- */
#ifndef _WIN32
#include <unistd.h>
#endif

static void relaunch(int role, const char *join, int port) {
    char value[160];
#ifdef _WIN32
    char exe[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
#define SETENV(n, v) SetEnvironmentVariableA(n, v)
#else
#ifdef __APPLE__
    char exe[1024];
#endif
#define SETENV(n, v) ((v) != NULL ? setenv(n, v, 1) : unsetenv(n))
#endif
    SETENV("BT3_NET_HOST", NULL);
    SETENV("BT3_NET_JOIN", NULL);
    SETENV("BT3_NET_SESSION", NULL);
    SETENV("BT3_SOUND_TICKS", NULL);
    if (role != 0) {
        if (role == 1) {
            snprintf(value, sizeof(value), "%d", port);
            SETENV("BT3_NET_HOST", value);
        } else {
            snprintf(value, sizeof(value), "%s:%d", join, port);
            SETENV("BT3_NET_JOIN", value);
        }
        SETENV("BT3_NET_SESSION", "1");
        SETENV("BT3_NET_NAME", sMyName);
        {   /* a match found with a room code: the lobby's local port, and for the host where to go on knocking */
            char text[64] = "";
            snprintf(value, sizeof(value), "%d", sCodeLocalPort);
            SETENV("BT3_NET_LOCALPORT", sCodeLocalPort != 0 ? value : NULL);
            if (sCodePunchSet) {
                inet_ntop(AF_INET, &sCodePunch.sin_addr, text, sizeof(text));
                snprintf(value, sizeof(value), "%s:%d", text, ntohs(sCodePunch.sin_port));
            }
            SETENV("BT3_NET_PUNCH", sCodePunchSet ? value : NULL);
        }
        if (role == 1) {
            snprintf(value, sizeof(value), "%u", (unsigned)sRules);
            SETENV("BT3_NET_RULES", value);
            snprintf(value, sizeof(value), "%d", sCount);
            SETENV("BT3_NET_PLAYERS", sCount > 2 ? value : NULL); /* (the others learn it from the host) */
        } else {
            SETENV("BT3_NET_RULES", NULL);
            SETENV("BT3_NET_PLAYERS", NULL);
        }
        SETENV("BT3_SOUND_TICKS", "1");
        SETENV("BT3_NOMOVIE", "1");
        if (getenv("BT3_NET_VOICE") == NULL) { /* (before the save folder changes: it is read from the player's own) */
            SETENV("BT3_NET_VOICE", own_voice_alt() ? "alt" : "default");
        }
        if (getenv("BT3_NET_OPTS") == NULL) { /* (the same for the player's camera type and screen shake) */
            snprintf(value, sizeof(value), "%u", (unsigned)(own_options() & 0xFFFFu));
            SETENV("BT3_NET_OPTS", value);
        }
        SETENV("BT3_SAVES", "net_session");
        /* the session's own save folder starts empty: both sides begin from the game's defaults */
        remove_session_save();
    } else {
        SETENV("BT3_SAVES", NULL); /* (a save folder named on the command line is not carried over a session: known) */
        SETENV("BT3_NOMOVIE", "1");
    }
    fflush(NULL);
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, sizeof(exe) - 1);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessA(exe, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        ExitProcess(0);
    }
#elif defined(__APPLE__)
    {
        extern int _NSGetExecutablePath(char *buf, uint32_t *size); /* <mach-o/dyld.h> */
        uint32_t cap = sizeof(exe) - 1;
        if (_NSGetExecutablePath(exe, &cap) == 0) {
            char *argv[2] = {exe, NULL};
            execv(exe, argv);
        }
    }
#else
    {
        char *argv[2] = {(char *)"/proc/self/exe", NULL};
        execv("/proc/self/exe", argv);
    }
#endif
    fprintf(stderr, "bt3: net: could not start the program again\n");
    exit(2);
}

/* 1 in a session started from Dragon Net Battle. */
int Port_NetSession(void) {
    return Port_NetActive() && sSession;
}

/* For the game's start-up (src/menu/menu_i_c.c, menu_a_c.c): 1 when it has to be the game's own whatever the
   player's settings say, which is in every session. BT3_TEST_OWN_START=1 (testing the check above): this copy
   follows its settings in a session too, as 0.1.32 and 0.1.33 did. */
int Port_NetFixedStart(void) {
    return Port_NetSession() && getenv("BT3_TEST_OWN_START") == NULL;
}

/* The session without starting the program again (gs/state.c exchanges the game's state; 64-bit programs).
   Begin: called on the game's own thread just before the game is put back to its first moment. */
extern int Port_SessionCan(void);                                    /* gs/state.c */
extern void Port_SessionRequest(int role, const char *address, int port);
extern void Port_SessionReturn(void);
static char sSavesBefore[512];
static int sSavesWasSet;

static void put_env(const char *name, const char *value) {
#ifdef _WIN32
    SetEnvironmentVariableA(name, value);
    _putenv_s(name, value != NULL ? value : "");
#else
    if (value != NULL) { setenv(name, value, 1); } else { unsetenv(name); }
#endif
}

void Port_NetSessionBegin(int role, const char *address, int port) {
    char host[16], join[160];
    static int known;

    if (!known) { /* the player's own save folder, as the program was started */
        const char *saves = getenv("BT3_SAVES");
        known = 1;
        sSavesWasSet = saves != NULL;
        snprintf(sSavesBefore, sizeof(sSavesBefore), "%s", saves != NULL ? saves : "");
    }
    /* the session's own save folder starts empty: both sides begin from the game's defaults */
    remove_session_save();
    sVoiceAlt = own_voice_alt(); /* (before the save folder changes: it is read from the player's own) */
    put_env("BT3_SAVES", "net_session");
    snprintf(host, sizeof(host), "%d", port);
    snprintf(join, sizeof(join), "%s:%d", address, port);
    if (sSock >= 0) {
        closesocket(sSock);
        sSock = -1;
    }
    sSession = 1;
    sArrived = 0;
    net_start(role, role == 1 ? host : NULL, role == 2 ? join : NULL);
}

/* How many sessions have ended in this run. The port's own memory: the game's is put back to the moment the session
   was asked for, so this is how the screens that asked for it (menu/port_tbo.c) know it has been and gone. */
static int sSessionsEnded;

int Port_NetSessionsEnded(void) {
    return sSessionsEnded;
}

void Port_NetSessionEnd(void) {
    sSessionsEnded++;
    Relay_Close(sSock);
    if (sSock >= 0) {
        closesocket(sSock);
        sSock = -1;
    }
    sMode = 0;
    sSession = 0;
    PortChat_Reset();
    put_env("BT3_SAVES", sSavesWasSet ? sSavesBefore : NULL);
}

/* The session's start-up runs without picture and sound and without waiting for real time until the game has
   reached the versus menu (headless.c says when). */
int Port_NetWarp(void) {
    return Port_NetSession() && !sArrived;
}
void Port_NetArrived(void) {
    sArrived = 1;
    fprintf(stderr, "bt3: net: the start-up is over at blank %u, %u ms after connecting: the character select\n", (unsigned)sTick, (unsigned)(now_ms() - sStartedMs));
}

/* The session is over (the game went back to the main menu, or the other side is gone): back to the game as it
   normally is. */
void Port_NetLeave(void) {
    fprintf(stderr, "bt3: net: the session has ended\n");
    PortChat_Reset();
    if (sSock >= 0 && sConnected) { /* tell the other side, so that it does not wait for input that will not come */
        uint32_t bye[3] = {MAGIC, T_BYE, sByeWhy};
        int k;
        for (k = 0; k < 3; k++) {
            int j;
            for (j = 1; j < (sMode == 1 ? sCount : 2); j++) { /* (the host: every other player) */
                net_send(bye, sizeof(bye), sMode == 1 ? peer_of(j) : &sPeer);
            }
        }
    }
    if (Port_SessionCan()) {
        Port_SessionReturn(); /* the game as it was when the session was asked for (does not return) */
    }
    relaunch(0, NULL, 0);
}

/* ---- the lobby: called from the settings code's window (ui.cpp), once a frame ---- */
static int sLobby; /* 0 idle, 1 waiting, 2 found, -1 failed */
static int sLobbyRole, sLobbyPort;
static char sLobbyAddr[128];
/* A room for more than two: the host's lobby takes greetings until everyone is there, and answers the ones who are
   there already with T_WAIT; then it answers them all at once and every copy begins its session. */
static struct sockaddr_in sGreeted[NMAX];
static uint32_t sGreetedId[NMAX]; /* (each copy's lobby greets with a number of its own: one player may come in by
                                     two ways, the home network's address and the public one, and counts once) */
static uint32_t sLobbyId;
static int sGreetedN, sWaitHave, sWaitOf;
static uint64_t sLobbyLast;

/* Room codes (net_match.c, port/matchmaking): the lobby's states 3 (asking the matchmaking service; the host's code
   is known at some point, Port_LobbyCode) and, once the other player's addresses are known, 1 as with an address:
   greetings go to every one of those addresses, and the host sends its own packets there too, so that both routers
   let the other side in. -3: the service said no (Port_LobbyError); -4: the two could not reach each other. */
static int sCodeMode, sCodeAddrs;
static int sCodeRelay = -1; /* which of sCodeAddr is the host's relay address (the one who joins) */
static int sCodeVia;        /* the match was reached through the relay (for the window) */
static struct sockaddr_in sCodeSeen; /* the address the service saw the other player under (for the relay's permission) */
static int sCodeSeenSet, sCodeGen, sCodeRelayGen; /* ... and which of the search's later news the lobby has taken in */

int Port_LobbyRelayed(void) { return sCodeVia; }
static struct sockaddr_in sCodeAddr[MATCH_ADDRS];
static char sCodeText[8], sCodeError[96];
static uint64_t sCodeSince;

const char *Port_LobbyCode(void) { return sCodeText; }
const char *Port_LobbyError(void) { return sCodeError; }

int Port_LobbyStart(int host, const char *address, int port) {
    struct sockaddr_in local;

    if (sHandSock >= 0) { /* (a session that was asked for and never began) */
        closesocket(sHandSock);
        sHandSock = -1;
    }
    if (sSock >= 0) {
        closesocket(sSock);
    }
#ifdef _WIN32
    {
        WSADATA w;
        u_long on = 1;
        WSAStartup(MAKEWORD(2, 2), &w);
        sSock = (int)socket(AF_INET, SOCK_DGRAM, 0);
        ioctlsocket(sSock, FIONBIO, &on);
    }
#else
    sSock = socket(AF_INET, SOCK_DGRAM, 0);
    fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL, 0) | O_NONBLOCK);
#endif
    sCodeMode = 0;
    sCodeVia = 0;
    sCodeLocalPort = 0;
    sCodePunchSet = 0;
    sLobbyRole = host ? 1 : 2;
    sLobbyPort = port;
    snprintf(sLobbyAddr, sizeof(sLobbyAddr), "%s", address != NULL ? address : "");
    memset(&sPeer, 0, sizeof(sPeer));
    sPeer.sin_family = AF_INET;
    sLobby = 1;
    sLobbyOther = 0;
    sLobbyLast = 0;
    sGreetedN = sWaitHave = sWaitOf = 0;
    sLobbyId = (uint32_t)SDL_GetPerformanceCounter() * 2654435761u ^ (uint32_t)SDL_GetTicksNS();
    if (host) {
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons((uint16_t)port);
        if (bind(sSock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            sLobby = -1;
        }
    } else {
        sPeer.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, sLobbyAddr, &sPeer.sin_addr) != 1) {
            struct hostent *he = gethostbyname(sLobbyAddr);
            if (he == NULL) {
                sLobby = -1;
            } else {
                memcpy(&sPeer.sin_addr, he->h_addr_list[0], sizeof(sPeer.sin_addr));
            }
        }
    }
    return sLobby;
}

static int sQuickQueue = -1; /* Port_LobbyStartQuick: the search that Port_LobbyStartCode starts is a quick match in this queue */

int Port_LobbyStartCode(int host, const char *code) {
    struct sockaddr_in local;
    socklen_t len = sizeof(local);

    Port_LobbyStart(1, "", 0); /* a socket on a port of the system's choosing */
    if (sLobby < 0) {
        return sLobby;
    }
    if (getsockname(sSock, (struct sockaddr *)&local, &len) != 0) {
        sLobby = -1;
        return sLobby;
    }
    sLobbyRole = host ? 1 : 2;
    sCodeMode = 1;
    sCodeAddrs = 0;
    sCodeSeenSet = 0;
    sCodeGen = sCodeRelayGen = 0;
    sCodeText[0] = sCodeError[0] = '\0';
    sCodeLocalPort = ntohs(local.sin_port);
    sLobbyPort = sCodeLocalPort;
    sLobby = 3;
    Relay_Close(-1);
    sCodeVia = 0;
    Match_Begin(sQuickQueue >= 0 ? 3 : sLobbyRole, sSock, sCodeLocalPort, sQuickQueue >= 0 ? kMatchQueues[sQuickQueue] : code, sMyName, NET_VERSION,
                sQuickQueue >= 0 ? 2 : sLobbyPlayers);
    return sLobby;
}

/* A quick match in one of the queues (net_match.h: kMatchQueues). Whether this player hosts or joins is the
   service's choice, known when it answers (Port_LobbyPoll takes it from there). */
int Port_LobbyStartQuick(int queue) {
    if (queue < 0 || queue >= MATCH_QUEUES) {
        return -1;
    }
    sQuickQueue = queue;
    sLobbyPlayers = 2; /* (quick matches are for two) */
    Port_LobbyStartCode(1, "");
    sQuickQueue = -1;
    return sLobby;
}

void Port_LobbyQueuesAsk(void) { Match_QueuesAsk(NET_VERSION); }
void Port_LobbyHelloAsk(void) { Match_HelloAsk(); }
int Port_LobbyRelease(int *min, int *latest) { return Match_Release(min, latest); }
int Port_LobbyQueueWaiting(int queue) { return Match_QueueWaiting(queue); }

void Port_LobbyCancel(void) {
    if (sCodeMode) {
        Match_Cancel();
        sCodeMode = 0;
    }
    Relay_Close(sSock);
    if (sSock >= 0) {
        closesocket(sSock);
        sSock = -1;
    }
    sLobby = 0;
    sLobbyPlayers = 2; /* (the next room is for two unless its host says otherwise) */
}

/* The other player's addresses as the matchmaking service gave them: where to knock, which of them is the host's
   relay address, and whom this side's relay (if it holds one) lets through. Called again when they change. */
static void lobby_take_addrs(const MatchStatus *st) {
    int i;

    sCodeAddrs = 0;
    sCodeRelay = -1;
    for (i = 0; i < st->peerCount && sCodeAddrs < MATCH_ADDRS; i++) {
        char text[64], *colon;
        const char *addr = st->peerAddr[i];
        struct sockaddr_in *a = &sCodeAddr[sCodeAddrs];
        memset(a, 0, sizeof(*a));
        a->sin_family = AF_INET;
        if (strncmp(addr, "seen=", 5) == 0) { /* the address the service saw the other player under: for the relay only */
            if (inet_pton(AF_INET, addr + 5, &a->sin_addr) == 1) {
                Relay_Permit(sSock, a);
                sCodeSeen = *a;
                sCodeSeenSet = 1;
            }
            continue;
        }
        if (strncmp(addr, "relay=", 6) == 0) { /* the host's relay address: tried when the direct ones stay silent */
            addr += 6;
            sCodeRelay = sCodeAddrs;
        }
        snprintf(text, sizeof(text), "%s", addr);
        colon = strrchr(text, ':');
        if (colon != NULL) {
            *colon = '\0';
            a->sin_port = htons((uint16_t)atoi(colon + 1));
            if (inet_pton(AF_INET, text, &a->sin_addr) == 1 && a->sin_port != 0) {
                Relay_Permit(sSock, a); /* (the host, if it holds a relay address: this player may come in through it) */
                sCodeAddrs++;
            } else if (sCodeRelay == sCodeAddrs) {
                sCodeRelay = -1;
            }
        }
    }
}

/* This side has got a relay address after the other player was known: he is let through it now. */
static void lobby_permit_again(void) {
    int i;
    for (i = 0; i < sCodeAddrs; i++) {
        if (i != sCodeRelay) {
            Relay_Permit(sSock, &sCodeAddr[i]);
        }
    }
    if (sCodeSeenSet) {
        Relay_Permit(sSock, &sCodeSeen);
    }
}

/* 0 idle, 1 still waiting, 2 the other player is there, -1 it cannot work (port in use, unknown address). */
int Port_LobbyPoll(void) {
    uint8_t pkt[256];
    struct sockaddr_in from;
    uint64_t now = SDL_GetTicksNS();
    int n;

    if (sLobby == 3) { /* a room code: what the matchmaking service has said so far */
        MatchStatus st;
        int i;
        Match_Poll(&st);
        if (st.state == 2) {
            Relay_Tick(sSock); /* (the relay address is kept while the room waits; the thread is done with the socket) */
        }
        snprintf(sCodeText, sizeof(sCodeText), "%s", st.code);
        if (st.role != 0) {
            sLobbyRole = st.role; /* (a quick match: host or joiner, as the service decided) */
        }
        if (st.state == -1) {
            snprintf(sCodeError, sizeof(sCodeError), "%s", st.error);
            sLobby = -3;
        } else if (st.state == 3) {
            lobby_take_addrs(&st);
            sCodeGen = st.addrGen;
            sCodeRelayGen = st.relayGen;
            sCodeSince = now;
            sLobby = sCodeAddrs > 0 ? 1 : -4;
        }
        return sLobby;
    }
    if (sLobby != 1) {
        return sLobby;
    }
    if (sCodeMode) {
        MatchStatus st;
        if (Match_SocketBusy()) {
            return sLobby; /* (the search's thread is getting a relay address on this socket: not touched meanwhile) */
        }
        Match_Poll(&st);
        if (st.state == 3 && st.addrGen != sCodeGen) { /* the host's relay address has come: knocked at from now on (the
                                                           host of a room for more: one more player has joined) */
            sCodeGen = st.addrGen;
            lobby_take_addrs(&st);
            if (sLobbyRole == 1) {
                sCodeSince = now;
            }
        }
        if (sLobbyRole == 1 && sLobbyPlayers > 2 && st.seats <= sGreetedN) {
            sCodeSince = now; /* (a room for more: no time limit while nobody who has joined it is still knocking) */
        }
        if (st.state == 3 && st.relayGen != sCodeRelayGen) { /* this side has a relay address now */
            sCodeRelayGen = st.relayGen;
            lobby_permit_again();
        }
    }
    if (sCodeMode && now - sLobbyLast > 200000000ull) {
        /* to every address the other player may be reached at: the one who joins greets, the host only knocks */
        uint32_t hello[4] = {MAGIC, T_HELLO, NET_VERSION, sLobbyId}, punch[2] = {MAGIC, T_PUNCH};
        int i;
        for (i = 0; i < sCodeAddrs; i++) {
            if (i == sCodeRelay ? now - sCodeSince < 3000000000ull && getenv("BT3_RELAY_ONLY") == NULL : getenv("BT3_RELAY_ONLY") != NULL) {
                continue; /* three seconds for the direct ways first (BT3_RELAY_ONLY, testing: the relay at once, and
                             nothing else, also while there is no relay address yet) */
            }
            if (sLobbyRole == 2) {
                net_send(hello, sizeof(hello), &sCodeAddr[i]);
            } else {
                net_send(punch, sizeof(punch), &sCodeAddr[i]);
            }
        }
        sLobbyLast = now;
        if (now - sCodeSince > 24000000000ull) {
            sLobby = -4; /* 24 seconds of knocking (a relay address is asked for after four of them, see net_match.c):
                            the two cannot reach each other */
            return sLobby;
        }
    } else if (!sCodeMode && sLobbyRole == 2 && now - sLobbyLast > 200000000ull) {
        uint32_t hello[4] = {MAGIC, T_HELLO, NET_VERSION, sLobbyId};
        net_send(hello, sizeof(hello), &sPeer);
        sLobbyLast = now;
    }
    while ((n = net_recv(pkt, (int)sizeof(pkt), &from)) >= 8) {
        uint32_t magic, type;
        memcpy(&magic, pkt, 4);
        memcpy(&type, pkt + 4, 4);
        if (magic != MAGIC) {
            continue;
        }
        if (sLobbyRole == 1 && type == T_HELLO && !(n >= 12 && version_is(pkt + 8))) {
            sLobbyOther = 1; /* someone with another release is trying: the window says so, and goes on waiting */
        } else if (sLobbyRole == 1 && type == T_HELLO) {
            uint32_t ack[3] = {MAGIC, T_HELLO_ACK, NET_VERSION};
            uint32_t id = from.sin_addr.s_addr ^ from.sin_port; /* (a lobby that says no number: its address) */
            int k, g;
            if (n >= 16) {
                memcpy(&id, pkt + 12, 4);
            }
            for (g = 0; g < sGreetedN && sGreetedId[g] != id; g++) {
            }
            if (g == sGreetedN) {
                if (sGreetedN + 1 >= sLobbyPlayers) {
                    continue; /* the room is full: not one of its players */
                }
                sGreetedId[sGreetedN] = id;
                sGreeted[sGreetedN++] = from;
                sCodeSince = now; /* (the knocking goes on for whoever is still to come) */
            }
            if (sGreetedN + 1 < sLobbyPlayers) {
                uint32_t wait[4] = {MAGIC, T_WAIT, (uint32_t)sGreetedN + 1, (uint32_t)sLobbyPlayers};
                net_send(wait, sizeof(wait), &from);
                continue;
            }
            for (k = 0; k < 3; k++) {
                for (g = 0; g < sGreetedN; g++) {
                    net_send(ack, sizeof(ack), &sGreeted[g]);
                }
            }
            if (sCodeMode) { /* where the session goes on knocking, so that the greeting of the session gets in */
                sCodePunch = from;
                sCodePunchSet = 1;
                sCodeVia = Relay_IsPeer(&from);
            }
            sLobby = 2;
        } else if (sLobbyRole == 2 && type == T_WAIT && n >= 16) {
            memcpy(&sWaitHave, pkt + 8, 4);
            memcpy(&sWaitOf, pkt + 12, 4);
            sCodeSince = now; /* (the host is there: the knocking goes on while its room fills) */
        } else if (sLobbyRole == 2 && type == T_HELLO_ACK) {
            sLobby = n >= 12 && version_is(pkt + 8) ? 2 : -2; /* -2: the host is another release */
            if (sCodeMode && sLobby == 2) { /* the address that answered is the one the session joins */
                inet_ntop(AF_INET, &from.sin_addr, sLobbyAddr, sizeof(sLobbyAddr));
                sLobbyPort = ntohs(from.sin_port);
                sCodeVia = sCodeRelay >= 0 && from.sin_addr.s_addr == sCodeAddr[sCodeRelay].sin_addr.s_addr && from.sin_port == sCodeAddr[sCodeRelay].sin_port;
            }
        }
    }
    return sLobby;
}

void Port_LobbyLaunch(void);
void Port_LobbyPlayersSet(int n);
int Port_LobbyCount(int *of);

/* BT3_LOBBY=host or BT3_LOBBY=join:CODE: the room-code lobby driven without its window, once a blank (tests; the
   code and each change of state are written to the log). BT3_RELAY_ONLY=1: the one who joins tries nothing but the
   host's relay address. */
static void lobby_background(void);
void Port_LobbyAuto(void) {
    static int state = -99, begun, coded;
    const char *what = getenv("BT3_LOBBY");
    int now;

    lobby_background();
    if (what == NULL || begun == 2 || Port_NetSession()) {
        return;
    }
    if (!begun) {
        begun = 1;
        if (getenv("BT3_NET_PLAYERS") != NULL) {
            Port_LobbyPlayersSet(atoi(getenv("BT3_NET_PLAYERS")));
        }
        if (strncmp(what, "quick:", 6) == 0) { /* BT3_LOBBY=quick:<queue>: a quick match (S, T, D10, D15, D20) */
            int q, type, dp;
            for (q = 0; q < MATCH_QUEUES && strcmp(what + 6, kMatchQueues[q]) != 0; q++) {
            }
            type = q == 0 ? 0 : q == 1 ? 1 : 2;
            dp = q >= 2 ? q - 2 : 0;
            Port_NetSpeedSet(NET_SPEED_STANDARD);
            Port_NetRulesSet(type, dp, 4); /* (a queue's rules: its battle type, no time limit, 120%) */
            if (getenv("BT3_NET_ROLLBACK") == NULL && getenv("BT3_NET_DELAY") == NULL) {
                Port_NetOptions(Port_RollCan() ? 8 : 0, -2); /* (and a quick match's fixed rollback and delay, as the window sets them) */
            }
            Port_LobbyStartQuick(q);
        } else {
            Port_LobbyStartCode(strncmp(what, "join:", 5) != 0, strncmp(what, "join:", 5) == 0 ? what + 5 : "");
        }
    }
    now = Port_LobbyPoll();
    {
        static int was;
        int of, here = Port_LobbyCount(&of);
        if (here != was && of > 2) {
            was = here;
            fprintf(stderr, "bt3: lobby: %d of %d players are here\n", here, of);
        }
    }
    if (now != state || (!coded && Port_LobbyCode()[0] != '\0')) {
        state = now;
        coded = Port_LobbyCode()[0] != '\0';
        fprintf(stderr, "bt3: lobby: state %d, code '%s'%s%s\n", now, Port_LobbyCode(), now == -3 ? ", " : "", now == -3 ? Port_LobbyError() : "");
    }
    if (now == 2) {
        fprintf(stderr, "bt3: lobby: the other player is there (%s)\n", Port_LobbyRelayed() ? "through the relay" : "directly");
        begun = 2;
        Port_LobbyLaunch();
    } else if (now < 0) {
        begun = 2;
    }
}

/* How many are to play in the match this copy hosts (2..NMAX; set before the lobby starts). */
void Port_LobbyPlayersSet(int n) {
    sLobbyPlayers = n < 2 ? 2 : n > NMAX ? NMAX : n;
}

/* A room for more than two: how many are there so far (the host counts itself), of how many (*of). */
int Port_LobbyCount(int *of) {
    *of = sLobbyRole == 1 ? sLobbyPlayers : sWaitOf;
    return sLobbyRole == 1 ? sGreetedN + 1 : sWaitHave;
}

/* The other player is there: start the session. */
void Port_LobbyLaunch(void) {
    if (sLobbyRole == 1) { /* (the session's host takes the number of players from here, also across a restart) */
        char value[8];
        snprintf(value, sizeof(value), "%d", sLobbyPlayers);
        put_env("BT3_NET_PLAYERS", sLobbyPlayers > 2 ? value : NULL);
        sCount = sLobbyPlayers;
        sLobbyPlayers = 2;
    }
    if (sCodeMode && Port_SessionCan()) {
        sHandSock = sSock; /* the session goes on with this socket (see sHandSock) */
    } else {
        closesocket(sSock);
    }
    sSock = -1;
    sLobby = 0;
    if (sCodeMode) {
        Match_Cancel();
        sCodeMode = 0;
        if (sLobbyRole == 1) {
            sLobbyPort = sCodeLocalPort; /* the host's session listens where the lobby did */
        }
    }
    if (Port_SessionCan()) {
        Port_SessionRequest(sLobbyRole, sLobbyAddr, sLobbyPort); /* the game's thread makes the change at its next blank */
        return;
    }
    relaunch(sLobbyRole, sLobbyAddr, sLobbyPort);
}

/* For the meter (ui.cpp). 0 when no match is on; else out = ping in ms (-1 not known yet), going back per second,
   blanks per going back x 10, waits per second, ms waited per second, the rollback limit (0: waiting for each
   other), the input delay. */
int Port_NetStats(int *out) {
    if (sMode <= 0 || !sConnected) {
        return 0;
    }
    out[0] = sPingMs;
    out[1] = sShown[1];
    out[2] = sShown[2];
    out[3] = sShown[3];
    out[4] = sShown[4];
    out[5] = sRollMax;
    out[6] = sDelay;
    out[7] = sAutoDelay;
    return 1;
}

/* 1 while a copy of another release is trying to join the lobby this copy hosts. */
int Port_LobbyOther(void) {
    return sLobbyOther;
}

/* ---- Tenkaichi Budokai Online (menu/port_tbo.c), the game's own screens for the room-code lobby ----
   The player's name is kept in online_name.txt beside the program. "Train While Waiting": the screens are left with
   the room still open; from then on Port_LobbyAuto (each blank) polls it, and when the other player comes the
   session begins wherever the game is (it starts from the game's first moment anyway). */
static int sLobbyBg;

const char *Port_NetNameGet(void) {
    static int loaded;
    if (!loaded) {
        FILE *f = fopen("online_name.txt", "rb");
        loaded = 1;
        if (f != NULL) {
            char line[64] = "";
            size_t n = fread(line, 1, sizeof(line) - 1, f);
            fclose(f);
            line[n] = 0;
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] != 0) {
                Port_NetNameSet(line);
            }
        }
    }
    return sMyName;
}

void Port_NetNameSave(const char *name) {
    FILE *f;
    Port_NetNameSet(name);
    f = fopen("online_name.txt", "wb");
    if (f != NULL) {
        fputs(sMyName, f);
        fclose(f);
    }
}

void Port_LobbyBackground(int on) {
    sLobbyBg = on;
}

int Port_LobbyInBackground(void) {
    return sLobbyBg;
}

static void lobby_background(void) {
    int now;
    if (!sLobbyBg || Port_NetSession()) {
        return;
    }
    now = Port_LobbyPoll();
    if (now == 2) {
        sLobbyBg = 0;
        Port_UiNotice("Tenkaichi Budokai Online: your opponent is here! The match begins.");
        Port_LobbyLaunch();
    } else if (now <= 0) {
        sLobbyBg = 0;
        Port_UiNotice(now == -3 ? Port_LobbyError() : "Tenkaichi Budokai Online: the room was closed.");
    }
}
