/*
 * Stage object animations (0x115170..0x115478), a part of the stage model code (0x114C60..0x115DE0).
 *
 * Member 6 of the stage file is an offset table (StgOfsTable) with three entries per animation: a model
 * resource, its VU1 packet node and a track whose length in frames is the u16 at +0xC. A stage object whose
 * definition has `anim` > 0 plays animation `anim` (1-based) in a loop while it is whole; objects with type bit
 * 0x40000000 keep playing after they are broken. The frame counter lives in the object DEFINITION (inside the
 * stage file), not in the runtime object. The drawing side (0x115478, 0x115DE0) reads it; nothing in the fight
 * simulation does.
 */
#include "common.h"
#include "sys/math3d.h"
#include "sys/randf.h"
#include "sys/vu1_packet.h"
#include "battle/stg_d.h"

/* Offset table of the stage file (StgOfsTable in battle/stg_a.h). */
typedef struct StgAnimTable {
    /* 0x00 */ s32 unk0;
    /* 0x04 */ s32 unk4;
    /* 0x08 */ s32 count;
    /* 0x0C */ s32 relocated;
    /* 0x10 */ s32 entries[1];
} StgAnimTable;

/* Third entry of an animation: only the length is read here. */
typedef struct StgAnimTrack {
    /* 0x00 */ u8 unk0[0xC];
    /* 0x0C */ u16 length;     /* frames */
} StgAnimTrack;

/* Local view of a stage object definition (StgObjDef in battle/stg_a.h, where +0x1C is still unknown). */
typedef struct StgAnimObjDef {
    /* 0x00 */ s32 hp;
    /* 0x04 */ s32 type;       /* bit 0x40000000: animated even when broken */
    /* 0x08 */ s32 anim;       /* 1-based animation number, <= 0 none */
    /* 0x0C */ s32 parent;
    /* 0x10 */ u8 unk10[0xC];
    /* 0x1C */ f32 frame;      /* current animation frame, written here at run time */
    /* 0x20 */ u8 unk20[0x20];
} StgAnimObjDef; /* size 0x40 */

/* Local views of the stage parameter block and of the stage object. */
typedef struct StgAnimData {
    /* 0x00 */ s32 unk0;
    /* 0x04 */ s32 unk4;
    /* 0x08 */ s32 objCount;
    /* 0x0C */ StgAnimObjDef *objDefs;
} StgAnimData;

typedef struct StgAnimStage {
    /* 0x00 */ s32 unk0;
    /* 0x04 */ StgAnimData *data;
} StgAnimStage;

/* Local view of the common resources: the stage file pointer (BattleRes.stage). */
typedef struct StgAnimRes {
    /* 0x00 */ u8 unk0[0x24];
    /* 0x24 */ u32 *stage;
} StgAnimRes;

/* Local view of the battle work: only the flag word. */
typedef struct StgAnimWork {
    /* 0x0000 */ u8 unk0[0x19F0];
    /* 0x19F0 */ u64 flags;    /* 0x100: paused */
} StgAnimWork;

extern StgAnimRes *gCommonRes;
extern StgAnimStage *gBtlStage;
/* 0x2FE914: this object's only .sdata word (stgm_a.c reads it too). */
StgAnimTable *gStgAnimTable = NULL;

extern StgAnimWork *Battle_GetWork(void);
extern void BtlStage_Init(void *stage, void *arg);
extern s32 BtlStage_IsObjBroken(s32 idx);
extern void StgOfsTable_Relocate(StgAnimTable *t);
extern void *StgOfsTable_Get(StgAnimTable *t, s32 i);
extern s32 StgOfsTable_GetCount(StgAnimTable *t);
extern void Res_RelocateOffsets(void *out, void *base, void *hdr);

/* Starts the stage: the stage object is the block at header word 1 of the stage file. */
void StgModel_InitStage(void) {
    u32 *file = gCommonRes->stage;
    void *stage = (u8 *)file + file[1] / 4 * 4;

    BtlStage_Init(stage, stage);
}

#ifdef PORT
#include <stdio.h>
#include "../../port/src/port_host.h"

/*
 * Big stages of the Tag Team Mod (SPEC-escala.md): every coordinate of a stage file just loaded, times 2 about the
 * origin, before anything in it is relocated or bound (its offsets are still the file's). As the mod: the arena
 * limits and the visibility distance, the zones and their collision meshes (boxes, vertices, plane distances; the
 * normals stay), the visibility tree, the batch spheres, the debris keys, the drawn vertices of every render group
 * but the sky (group 0), the parameter block's objects, sun, starts, other starts and places, and the intro
 * cameras. Beyond the mod (the computer and the camera use them): the navigation graph, the stage paths and the
 * water level. Stages with moving scenery stay as they are, and so do those a stage change turns into one of them;
 * so does a file the plan does not know. The plan marks every word to scale (once each, however many tables share
 * it) and nothing changes unless all of it holds.
 */
#define STGBIG_MAX 0x6CB800 /* BTL_STAGE_BUF_SIZE: larger files (BT3_STAGE_BUF) stay as they are */
PORT_HOST static u32 sBigMark[STGBIG_MAX / 128];  /* a bit per word of the file: to scale */
PORT_HOST static u32 sBigSeen[STGBIG_MAX / 128];  /* visibility tree nodes walked, sky packets, mesh nodes done */
PORT_HOST static u32 sBigOpen[STGBIG_MAX / 128];  /* mesh nodes on the walk's path (a loop: refused) */
PORT_HOST static u32 sBigStack[0x10000];
PORT_HOST static u8 *sBigFile;
PORT_HOST static u32 sBigSize;
PORT_HOST static s32 sBigBad;
s32 gPortStageBig; /* the stage now loaded was made big (a stage change keeps it so) */

#define BIG_BIT(bits, at) ((bits)[(at) >> 7] & (1u << (((at) >> 2) & 31)))
#define BIG_SET(bits, at) ((bits)[(at) >> 7] |= 1u << (((at) >> 2) & 31))
#define BIG_CLR(bits, at) ((bits)[(at) >> 7] &= ~(1u << (((at) >> 2) & 31)))

static u32 StgBig_U32(u32 at) {
    if ((at & 3) != 0 || at > sBigSize - 4) {
        sBigBad = 1;
        return 0;
    }
    return *(u32 *)(sBigFile + at);
}

/* Whether n numbers at `at`, inside [lo, hi), are sane coordinates (finite, under 1e8); marks them to scale too. */
static s32 StgBig_Sane(u32 at, u32 n, u32 lo, u32 hi, s32 mark) {
    u32 i;

    if (at < lo || at > hi || n > (hi - at) / 4 || (at & 3) != 0) {
        sBigBad = 1;
        return 0;
    }
    for (i = 0; i < n; i++, at += 4) {
        f32 v = *(f32 *)(sBigFile + at);

        if (!(v > -1e8f && v < 1e8f)) {
            sBigBad = 1;
            return 0;
        }
        if (mark) {
            BIG_SET(sBigMark, at);
        }
    }
    return 1;
}

/* n coordinates at `at`, inside [lo, hi), to scale. */
static void StgBig_Vec(u32 at, u32 n, u32 lo, u32 hi) {
    StgBig_Sane(at, n, lo, hi, 1);
}

/* A table of `count` rows of `stride` bytes at offset `ptr` of the block [base, base + len). */
static s32 StgBig_Rows(u32 ptr, u32 count, u32 stride, u32 len) {
    if (count > 1000000 || ptr > len || (u64)count * stride > len - ptr || (count != 0 && ptr < 96)) {
        sBigBad = 1;
        return 0;
    }
    return 1;
}

/* One collision mesh of the root block (header at R + at): its boxes, plane distances and vertices. */
static void StgBig_Mesh(u32 R, u32 L, u32 at) {
    u32 nodes, tris, nodeAt, triAt, maxIndex = 0, i, top;

    if (at < 96 || at > L - 48) {
        sBigBad = 1;
        return;
    }
    nodes = StgBig_U32(R + at);
    tris = StgBig_U32(R + at + 4);
    nodeAt = StgBig_U32(R + at + 12) * 4;
    triAt = StgBig_U32(R + at + 16) * 4;
    if (nodes == 0 || nodes > 1000000 || tris == 0 || tris > 1000000 || !StgBig_Rows(nodeAt, nodes, 32, L) ||
        !StgBig_Rows(triAt, tris, 32, L)) {
        sBigBad = 1;
        return;
    }
    StgBig_Vec(R + at + 24, 6, R, R + L);
    for (i = 0; i < nodes && !sBigBad; i++) {
        u32 node = R + nodeAt + i * 32;
        u32 left = StgBig_U32(node + 24), right = StgBig_U32(node + 28);

        StgBig_Vec(node, 6, R, R + L);
        if (sBigBad || !(*(f32 *)(sBigFile + node) <= *(f32 *)(sBigFile + node + 12)) ||
            !(*(f32 *)(sBigFile + node + 4) <= *(f32 *)(sBigFile + node + 16)) ||
            !(*(f32 *)(sBigFile + node + 8) <= *(f32 *)(sBigFile + node + 20)) ||
            (right == 0xFFFFFFFF ? left >= tris : (left >= nodes || right >= nodes))) {
            sBigBad = 1;
        }
    }
    /* the tree from node 0 must not loop (the game's walk would never end); bit 31: leaving the node */
    top = 0;
    sBigStack[top++] = 0;
    while (top > 0 && !sBigBad) {
        u32 e = sBigStack[--top], node = R + nodeAt + (e & 0x7FFFFFFF) * 32;

        if (e & 0x80000000) {
            BIG_CLR(sBigOpen, node);
            BIG_SET(sBigSeen, node);
        } else if (!BIG_BIT(sBigSeen, node)) {
            if (BIG_BIT(sBigOpen, node) || top + 3 > sizeof(sBigStack) / sizeof(sBigStack[0])) {
                sBigBad = 1;
                return;
            }
            BIG_SET(sBigOpen, node);
            sBigStack[top++] = e | 0x80000000;
            if (StgBig_U32(node + 28) != 0xFFFFFFFF) {
                sBigStack[top++] = StgBig_U32(node + 24);
                sBigStack[top++] = StgBig_U32(node + 28);
            }
        }
    }
    for (i = 0; i < tris && !sBigBad; i++) {
        u32 tri = R + triAt + i * 32, k;

        for (k = 4; k <= 12; k += 4) {
            if (StgBig_U32(tri + k) > maxIndex) {
                maxIndex = StgBig_U32(tri + k);
            }
        }
        StgBig_Sane(tri + 16, 3, R, R + L, 0); /* (the normal stays) */
        StgBig_Vec(tri + 28, 1, R, R + L);
    }
    if (maxIndex >= 1000000 || !StgBig_Rows(StgBig_U32(R + at + 20) * 4, maxIndex + 1, 16, L)) {
        sBigBad = 1;
        return;
    }
    for (i = 0; i <= maxIndex && !sBigBad; i++) {
        StgBig_Vec(R + StgBig_U32(R + at + 20) * 4 + i * 16, 3, R, R + L);
    }
}

/* The drawn vertices of a render packet (a DMA chain of VIF codes: position, colour, uv per vertex). */
static void StgBig_Packet(u32 R, u32 L, u32 packet) {
    u32 tag = StgBig_U32(R + packet), end, cur;

    end = packet + 16 + (tag & 0xFFFF) * 16;
    if (((tag >> 28) & 7) != 6 || end > L) {
        sBigBad = 1;
        return;
    }
    for (cur = packet + 8; cur < end && !sBigBad;) {
        u32 word = StgBig_U32(R + cur), cmd = (word >> 24) & 0x7F, n = (word >> 16) & 0xFF, imm = word & 0xFFFF, v;

        cur += 4;
        if (cmd == 0) {
            continue;
        }
        if (cmd == 0x6C) { /* UNPACK V4-32 */
            if (n == 0 || cur + n * 16 > end) {
                sBigBad = 1;
            } else if (imm == 0x8003) {
                if (n % 3 != 0) {
                    sBigBad = 1;
                }
                for (v = cur; v < cur + n * 16 && !sBigBad; v += 48) {
                    StgBig_Vec(R + v, 3, R, R + L);
                }
            } else if (imm != 0x8000 || (n != 1 && n != 3)) {
                sBigBad = 1;
            }
            cur += n * 16;
        } else if (cmd != 0x17) { /* MSCNT */
            sBigBad = 1;
        }
    }
    if (cur != end) {
        sBigBad = 1;
    }
}

/* The render groups' packets: group 0 (the sky) only remembered, the others' vertices marked. */
static void StgBig_Groups(u32 R, u32 L, s32 world) {
    u32 count = StgBig_U32(R + 24), at = StgBig_U32(R + 28) * 4, g, m, d;

    if (!StgBig_Rows(at, count, 8, L)) {
        return;
    }
    for (g = world ? 1 : 0; g < (world ? count : (count != 0)) && !sBigBad; g++) {
        u32 group = R + at + g * 8, mats = *(u16 *)(sBigFile + group + 2), matAt = StgBig_U32(group + 4) * 4;

        if (!StgBig_Rows(matAt, mats, 16, L)) {
            return;
        }
        for (m = 0; m < mats && !sBigBad; m++) {
            u32 mat = R + matAt + m * 16, draws = *(u16 *)(sBigFile + mat + 2), drawAt = StgBig_U32(mat + 4) * 4;

            if (!StgBig_Rows(drawAt, draws, 16, L)) {
                return;
            }
            for (d = 0; d < draws && !sBigBad; d++) {
                u32 packet = StgBig_U32(R + drawAt + d * 16 + 8) * 4;

                if (packet > L - 16) {
                    sBigBad = 1;
                } else if (!world) {
                    BIG_SET(sBigSeen, R + packet);
                } else if (BIG_BIT(sBigSeen, R + packet)) {
                    sBigBad = 1; /* a packet both sky and world */
                } else {
                    StgBig_Packet(R, L, packet);
                }
            }
        }
    }
}

/* Marks what to scale in the stage file. Returns NULL when it all holds, else why the stage stays as it is. */
static const char *StgBig_Plan(void) {
    static const u8 kMapd[5][4] = {{8, 12, 64, 32}, {24, 28, 32, 0}, {40, 44, 32, 0}, {48, 52, 32, 0}, {56, 60, 32, 0}};
    u32 count = StgBig_U32(0), i, j, R, L, M, ML, n, at;
    u32 stack[512];
    s32 top;

    if (count == 0 || count > 4096 || 4 * (count + 2) > sBigSize) {
        return "not a stage file";
    }
    for (i = 0; i <= count; i++) {
        u32 o = StgBig_U32(4 + 4 * i) & ~3u;

        if ((i == 0 && o < 4 * (count + 2)) || (i > 0 && o < (StgBig_U32(4 * i) & ~3u))) {
            return "not a stage file";
        }
    }
    sBigSize = StgBig_U32(4 + 4 * count) & ~3u; /* the package's end */
#define OFS(e) (StgBig_U32(4 + 4 * (e)) & ~3u)
    if (count < 2 || sBigBad) {
        return "not a stage file";
    }
    if (sBigSize > STGBIG_MAX) {
        return "too large";
    }
    R = OFS(0);
    L = OFS(1) - R;
    M = OFS(1);
    ML = OFS(2) - M;
    if (L < 96 || StgBig_U32(R) != 18 || StgBig_U32(R + 4) != 0) {
        return "not a stage file";
    }
    if (StgBig_U32(R + 16) != 0 || (count > 8 && OFS(9) > OFS(8))) {
        return "moving scenery";
    }
    /* arena limits (radius, top, bottom) and the visibility distance */
    at = StgBig_U32(R + 64) * 4;
    StgBig_Vec(R + at, 3, R, R + L);
    if (sBigBad || !(*(f32 *)(sBigFile + R + at) > 0.0f) ||
        !(*(f32 *)(sBigFile + R + at + 4) < *(f32 *)(sBigFile + R + at + 8))) {
        return "no arena limits";
    }
    StgBig_Vec(R + 76, 1, R, R + L);
    /* zones: radius, centre, half extents; their collision meshes (and those of their extra records) */
    n = StgBig_U32(R + 56);
    at = StgBig_U32(R + 60) * 4;
    if (n == 0 || n > 16384 || !StgBig_Rows(at, n, 64, L)) {
        return "no zones";
    }
    for (i = 0; i < n && !sBigBad; i++) {
        u32 zone = R + at + i * 64, extra = StgBig_U32(zone + 24) * 4, extras = StgBig_U32(zone + 20);
        u32 near = StgBig_U32(zone + 12) * 4, nears = StgBig_U32(zone + 8);

        StgBig_Vec(zone + 28, 1, R, R + L);
        StgBig_Vec(zone + 32, 3, R, R + L);
        StgBig_Vec(zone + 48, 3, R, R + L);
        if (sBigBad || !(*(f32 *)(sBigFile + zone + 48) >= 0.0f) || !(*(f32 *)(sBigFile + zone + 52) >= 0.0f) ||
            !(*(f32 *)(sBigFile + zone + 56) >= 0.0f) || !StgBig_Rows(near, nears, 4, L)) {
            return "bad zones";
        }
        for (j = 0; j < nears; j++) {
            if (StgBig_U32(R + near + j * 4) >= n) {
                return "bad zones";
            }
        }
        if (StgBig_U32(zone + 4) != 0) {
            StgBig_Mesh(R, L, StgBig_U32(zone + 4) * 4);
        }
        if (StgBig_Rows(extra, extras, 16, L)) {
            for (j = 0; j < extras && !sBigBad; j++) {
                StgBig_Mesh(R, L, StgBig_U32(R + extra + j * 16 + 8) * 4); /* (must be there) */
            }
        }
    }
    /* the other tables the game walks must lie inside the block */
    {
        static const u8 kTables[6][3] = {{16, 20, 16}, {24, 28, 8}, {32, 36, 8}, {40, 44, 1}, {48, 52, 80}, {68, 72, 80}};

        for (i = 0; i < 6; i++) {
            StgBig_Rows(StgBig_U32(R + kTables[i][1]) * 4, StgBig_U32(R + kTables[i][0]), kTables[i][2], L);
        }
    }
    /* the parameter block: its tag, the two players' starts, an environment record */
    if (ML < 168 || StgBig_U32(M) != 0x4450414D || StgBig_U32(M + 40) != 2 ||
        !StgBig_Rows(StgBig_U32(M + 44), 2, 32, ML) || StgBig_U32(M + 16) == 0 ||
        !StgBig_Rows(StgBig_U32(M + 20), StgBig_U32(M + 16), 16, ML) ||
        !StgBig_Sane(M + StgBig_U32(M + 44), 16, M, M + ML, 0)) {
        return "unknown parameter block";
    }
    if (sBigBad) {
        return "unknown model data";
    }
    /* visibility tree: each node's centre; children at +4..+0x20 */
    top = 0;
    stack[top++] = StgBig_U32(R + 80) * 4;
    for (n = 0; top > 0 && !sBigBad;) {
        at = stack[--top];
        if (at < 96 || at > L - 64 || BIG_BIT(sBigSeen, R + at) || ++n > 100000) {
            return "bad visibility tree";
        }
        BIG_SET(sBigSeen, R + at);
        StgBig_Vec(R + at + 40, 3, R, R + L);
        for (j = 1; j <= 8; j++) {
            if (StgBig_U32(R + at + 4 * j) != 0) {
                if (top == 512) {
                    return "bad visibility tree";
                }
                stack[top++] = StgBig_U32(R + at + 4 * j) * 4;
            }
        }
    }
    /* batch spheres */
    n = StgBig_U32(R + 32);
    at = StgBig_U32(R + 36) * 4;
    if (StgBig_Rows(at, n, 8, L)) {
        for (i = 0; i < n && !sBigBad; i++) {
            u32 batches = StgBig_U32(R + at + i * 8), batchAt = StgBig_U32(R + at + i * 8 + 4) * 4;

            if (StgBig_Rows(batchAt, batches, 32, L)) {
                for (j = 0; j < batches; j++) {
                    StgBig_Vec(R + batchAt + j * 32 + 12, 4, R, R + L);
                }
            }
        }
    }
    /* debris keys: a position each */
    n = StgBig_U32(R + 48);
    at = StgBig_U32(R + 52) * 4;
    if (StgBig_Rows(at, n, 80, L)) {
        for (i = 0; i < n && !sBigBad; i++) {
            u32 keys = StgBig_U32(R + at + i * 80 + 64), keyAt = StgBig_U32(R + at + i * 80 + 68) * 4;

            if (StgBig_Rows(keyAt, keys, 32, L)) {
                for (j = 0; j < keys; j++) {
                    StgBig_Vec(R + keyAt + j * 32, 3, R, R + L);
                }
            }
        }
    }
    StgBig_Groups(R, L, 0);
    StgBig_Groups(R, L, 1);
    if (sBigBad) {
        return "unknown model data";
    }
    /* the parameter block: objects, sun, starts, other starts, places (byte offsets from the block) */
    for (i = 0; i < 5; i++) {
        u32 rows = StgBig_U32(M + kMapd[i][0]), rowAt = StgBig_U32(M + kMapd[i][1]);

        if (StgBig_Rows(rowAt, rows, kMapd[i][2], ML)) {
            for (j = 0; j < rows; j++) {
                StgBig_Vec(M + rowAt + j * kMapd[i][2] + kMapd[i][3], 3, 0, sBigSize);
                if (i >= 2) { /* (a place: where to stand, then what to face) */
                    StgBig_Vec(M + rowAt + j * kMapd[i][2] + 16, 3, 0, sBigSize);
                }
            }
        }
    }
    /* the water level of each environment record (beyond the mod) */
    n = StgBig_U32(M + 0x10);
    at = StgBig_U32(M + 0x14);
    if (StgBig_Rows(at, n, 16, ML)) {
        for (i = 0; i < n; i++) {
            StgBig_Vec(M + at + i * 16 + 8, 1, 0, sBigSize);
        }
    }
    /* intro cameras (entries 2..4, "CMAn"): the start position and the keys of the three position channels */
    for (i = 2; i <= 4 && i < count && !sBigBad; i++) {
        u32 T = OFS(i), TL = OFS(i + 1) - T, rowAt, rows, seen = 0;

        if (TL == 0) {
            continue;
        }
        if (TL < 48 || StgBig_U32(T) != 0x6E414D43 || StgBig_U32(T + 4) != 6 || StgBig_U32(T + 12) == 0 ||
            StgBig_U32(T + 12) > 8) {
            return "unknown intro camera";
        }
        StgBig_Vec(T + StgBig_U32(T + 24), 3, 0, sBigSize);
        rowAt = StgBig_U32(T + 8);
        rows = StgBig_U32(T + 12);
        if (rowAt < 48 || rowAt > TL || rows * 16 > TL - rowAt) {
            return "unknown intro camera";
        }
        for (j = 0; j < rows && !sBigBad; j++) {
            u32 ch = StgBig_U32(T + rowAt + j * 16), keyAt = StgBig_U32(T + rowAt + j * 16 + 4);
            u32 keys = StgBig_U32(T + rowAt + j * 16 + 8), k;

            if (ch >= 8 || (seen & (1u << ch)) || keyAt < 48 || keyAt > TL || keys >= 100000 || keys * 32 > TL - keyAt) {
                return "unknown intro camera";
            }
            seen |= 1u << ch;
            for (k = 0; ch < 3 && k < keys; k++) {
                StgBig_Vec(T + keyAt + k * 32 + 8, 1, 0, sBigSize);
            }
        }
    }
    /* the computer's navigation graph (entry 15, beyond the mod): a position per node */
    if (count > 16 && OFS(16) > OFS(15)) {
        u32 N = OFS(15), NL = OFS(16) - N;

        n = StgBig_U32(N + 4);
        if (NL < 16 || StgBig_U32(N) != 1 || n > (NL - 16) / 0x30) {
            return "unknown navigation graph";
        }
        for (i = 0; i < n; i++) {
            StgBig_Vec(N + 16 + i * 0x30, 3, N, N + NL);
        }
    }
    /* the stage paths (entry 23, beyond the mod): offsets from the table, not relocated yet */
    if (count > 24 && OFS(24) - OFS(23) >= 0x14) {
        u32 P = OFS(23), PL = OFS(24) - P, paths = StgBig_U32(P + 0xC);

        n = StgBig_U32(P + 8);
        if (StgBig_U32(P + 0x10) != 0 || paths > PL || n > (PL - paths) / 16) {
            return "unknown stage paths";
        }
        for (i = 0; i < n && !sBigBad; i++) {
            u32 pts = StgBig_U32(P + paths + i * 16 + 4), k;

            j = StgBig_U32(P + paths + i * 16);
            if (pts > PL || j > (PL - pts) / 0x30) {
                return "unknown stage paths";
            }
            for (k = 0; k < j; k++) {
                StgBig_Vec(P + pts + k * 0x30, 3, P, P + PL);
            }
        }
    }
#undef OFS
    return sBigBad ? "unknown data" : NULL;
}

/* Battle_Init and a stage change (BtlLoad_EndStageSwap), before StgModel_InitStage: the stage file just loaded
   made big when big stages are on (gPortBigStages, port/src/multi_fighter.c) and the plan holds; after a stage
   change only when the stage before was big too. A stage that a change turns into one that stays as it is stays as it
   is too (change kind 1 leads to stage 15, 2 to stage 3; BT4: the record's byte 13; BtlStage_GetChangeTarget). */
void BtlStage_PortBig(s32 change) {
    /* the stages that stay as they are (research/tests/stage_sweep.py, 2026-10-20; BT4: research/bt4-rea/stage_big.py) */
    static const u8 kKept[] = {4, 5, 8, 12, 17, 18, 27, 0xFF};
    static const u8 kKeptBt4[] = {4, 5, 8, 11, 12, 18, 21, 23, 27, 40, 46, 47, 52, 57, 58, 0xFF};
    extern int gPortBigStages;
    extern s32 Battle_GetStage(void);
    extern void *memset(void *dst, s32 c, u32 n);
    extern void Port_StageDump(int stage, int after, u32 addr, int size); /* port/src/headless.c (tests) */
    u8 *file = (u8 *)gCommonRes->stage;
    const char *why = NULL;
    u32 i;

    if (file == NULL || !gPortBigStages || (change && !gPortStageBig)) {
        gPortStageBig = 0;
        return;
    }
    sBigFile = file;
    sBigSize = STGBIG_MAX;
    sBigBad = 0;
    memset(sBigMark, 0, sizeof(sBigMark));
    memset(sBigSeen, 0, sizeof(sBigSeen));
    memset(sBigOpen, 0, sizeof(sBigOpen));
    why = StgBig_Plan();
    if (why == NULL && !change) { /* (the parameter block's first environment record: its change kind) */
        u32 M = StgBig_U32(8) & ~3u;
        u32 kind = StgBig_U32(M + 0x10) == 0 ? 0 : StgBig_U32(M + StgBig_U32(M + 0x14) + 0xC);

        s32 target = gPortBt4 ? (s8)(kind >> 8) : kind == 1 ? 15 : kind == 2 ? 3 : -1; /* (BT4: REA 0x24349C) */
        const u8 *kept = kKept;

        if (gPortBt4) { /* (not ?: between the two: ptr32) */
            kept = kKeptBt4;
        }

        for (i = 0; target >= 0 && kept[i] != 0xFF; i++) {
            if (kept[i] == target) {
                why = "a stage change turns it into one that stays as it is";
            }
        }
    }
    gPortStageBig = why == NULL;
    Port_StageDump(Battle_GetStage(), 0, (u32)file, sBigSize);
    printf("bt3: stage %d %s%s\n", (int)Battle_GetStage(), why == NULL ? "made big" : "stays as it is: ",
           why == NULL ? "" : why);
    if (why != NULL) {
        return;
    }
    for (i = 0; i < sBigSize; i += 4) {
        if (BIG_BIT(sBigMark, i)) {
            *(f32 *)(file + i) *= 2.0f;
        }
    }
    Port_StageDump(Battle_GetStage(), 1, (u32)file, sBigSize);
}
#endif

/* Gives every animated, whole object a random start frame in its animation. */
void StgModel_ResetAnims(void) {
    StgAnimData *data;
    StgAnimObjDef *def;
    s32 i;
    s32 anim;

    if (gStgAnimTable == NULL) {
        return;
    }
    data = gBtlStage->data;
    def = data->objDefs;
    for (i = 0; i < data->objCount; i++) {
        anim = def->anim;
        if (anim <= 0) {
            def++;
            continue;
        }
        if (!(def->type & 0x40000000) && BtlStage_IsObjBroken(i)) {
            def++;
            continue;
        }
        {
            StgAnimTrack *track = StgOfsTable_Get(gStgAnimTable, anim * 3 - 1);

            def->frame = Rand_FloatRange(0.0f, 1.0f) * (f32)track->length;
        }
        def++;
    }
}

/* Binds the animation table (member 6 of the stage file) and prepares each animation's model and packet. */
void StgModel_BindAnims(void) {
    u32 *file = gCommonRes->stage;
    void *res;
    u32 *node;
    s32 i;
    s32 count;

    if (file == NULL) {
        return;
    }
    if (file[10] == file[9]) {
        gStgAnimTable = NULL;
        return;
    }
    gStgAnimTable = (StgAnimTable *)((u8 *)file + file[9] / 4 * 4);
    StgOfsTable_Relocate(gStgAnimTable);
    count = StgOfsTable_GetCount(gStgAnimTable);
    for (i = 0; i < count; i += 3) {
        res = StgOfsTable_Get(gStgAnimTable, i);
        Res_RelocateOffsets(&res, res, res);
        node = StgOfsTable_Get(gStgAnimTable, i + 1);
        Vu1Node_OrFlags((Vu1Node *)((u8 *)node + node[0] / 4 * 4), 0x3480, 0x3700);
    }
    StgModel_ResetAnims();
}

/* Forgets the animation table. */
void StgModel_ClearAnims(void) {
    gStgAnimTable = NULL;
}

/* Per frame: advances the animation of every animated object that is whole by 2 frames, looping. Not while paused. */
void StgModel_UpdateAnims(void) {
    StgAnimData *data;
    StgAnimObjDef *def;
    s32 i;
    s32 anim;
    StgAnimTrack *track;

    if (gStgAnimTable == NULL) {
        return;
    }
    if (Battle_GetWork()->flags & 0x100) {
        return;
    }
    data = gBtlStage->data;
    def = data->objDefs;
    for (i = 0; i < data->objCount; i++) {
        anim = def->anim;
        if (anim <= 0) {
            def++;
            continue;
        }
        if (!(def->type & 0x40000000) && BtlStage_IsObjBroken(i)) {
            def++;
            continue;
        }
        def->frame += 2.0f;
        track = StgOfsTable_Get(gStgAnimTable, anim * 3 - 1);
        if ((f32)track->length < def->frame) {
            def->frame = 0.0f;
        }
        def++;
    }
}
