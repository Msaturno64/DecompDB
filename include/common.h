#ifndef COMMON_H
#define COMMON_H

#include "include_asm.h"
#include "types.h"

#define NULL 0

#ifdef PORT
/* A number the BT4 disc changed (TeamBT4, PRIVATE; each one read with REA): `bt3` with BT3's data, `bt4` with BT4's. */
extern int gPortBt4;
#define PORT_BT4(bt3, bt4) (gPortBt4 ? (bt4) : (bt3))
#else
#define PORT_BT4(bt3, bt4) (bt3)
#endif

#endif
