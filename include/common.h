#ifndef COMMON_H
#define COMMON_H

#include "include_asm.h"
#include "types.h"

#define NULL 0

#ifdef PORT
/* A number the BT4 disc changed (TeamBT4, PRIVATE; each one read with REA): `bt3` with BT3's data, `bt4` with BT4's. */
extern int gPortBt4;
#define PORT_BT4(bt3, bt4) (gPortBt4 ? (bt4) : (bt3))
/* BT4's English disc (B14 REV2 ENG): B14 REV2 with a few of its changes undone and its own music ids (REA, the two
   discs' programs compared: research/bt4-rea/diff_eng.py). gPortBt4 is set with it too. */
extern int gPortBt4Eng;
#define PORT_BT4E(bt3, bt4, eng) (gPortBt4 ? (gPortBt4Eng ? (eng) : (bt4)) : (bt3))
#else
#define PORT_BT4(bt3, bt4) (bt3)
#define PORT_BT4E(bt3, bt4, eng) (bt3)
#define gPortBt4 0 /* (the PS2 build: BT3 only) */
#define gPortBt4Eng 0
#endif

#endif
