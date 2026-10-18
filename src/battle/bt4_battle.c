#include "common.h"
#ifdef PORT
/*
 * The BT4 disc's battle code that is not BT3's (TeamBT4 B14 REV2; PRIVATE: never published). Each function was read
 * with REA (research/rea-docker) from BT4's SLUS_219.78 and DBZP.BIN; the addresses in the notes are BT4's. Used only
 * with gPortBt4 (port/src/plat_mem.c), from the game code that BT4 patched (research/bt4-rea/hooks.md).
 *
 * A character's data that BT3 keeps in its model pack, BT4 loads from three files of its own for each fighter
 * (0x200900): its sections (0x1036 + character, 0xA800 bytes), one more (0x1130 + character, 0x4000) and its lip
 * tracks in the voice language (0x122A or 0x1324 + character, 0x5000), in a table of rows by kind and columns by
 * fixed model slot; the third row has one more column, for the first object slot (BtlLoad_StepObject).
 */
#include "battle/battle_setup.h"
#include "battle/btl_obj.h"
#include "battle/view_a.h"
#include "sys/common.h"
#include "sys/heap.h"
#include "sys/save.h"

extern void *File_Request3(s32 id, void *buf, s32 size) __asm__("File_Request");
extern s32 BtlScene_GetPackEntrySize(void *pack, s32 n);
extern void *BtlScene_GetPackEntry(void *pack, s32 n);

#define BT4_DATA_ROWS 3
#define BT4_DATA_OBJECT BTL_RES_FIXED_MAX /* the column of the first object slot (BT4's slot number 2) */

static void *sBt4Data[BT4_DATA_ROWS][BTL_RES_FIXED_MAX + 1];
static const s32 kBt4DataSize[BT4_DATA_ROWS] = { 0xA800, 0x4000, 0x5000 };

/* The file of row `row` for a character. */
static s32 Bt4Data_File(s32 row, s32 chara) {
    if (row == 0) {
        return 0x1036 + chara;
    }
    if (row == 1) {
        return 0x1130 + chara;
    }
    return ((gSaveData->flags & SAVE_FLAG_VOICE) ? 0x1324 : 0x122A) + chara;
}

/* The buffers and the first load, for every fighter of the battle (0x200900; BT4 has two). */
void Bt4Data_LoadAll(void) {
    s32 row;
    s32 side;

    memset(sBt4Data, 0, sizeof(sBt4Data));
    for (row = 0; row < BT4_DATA_ROWS; row++) {
        for (side = 0; side < BTL_SIDE_COUNT; side++) {
            sBt4Data[row][side] = Heap_Alloc(kBt4DataSize[row], 0x40, 0, HEAP_ANY);
        }
    }
    sBt4Data[2][BT4_DATA_OBJECT] = Heap_Alloc(kBt4DataSize[2], 0x40, 0, HEAP_ANY);
    for (row = 0; row < BT4_DATA_ROWS; row++) {
        for (side = 0; side < BTL_SIDE_COUNT; side++) {
            File_Request3(Bt4Data_File(row, BattleSide_GetStartChara(side)), sBt4Data[row][side], kBt4DataSize[row]);
        }
    }
}

/* A side's new character (0x2009F0, BtlLoad_StepChara): its three files into its model slot's column. */
void Bt4Data_LoadSide(s32 side, s32 chara) {
    s32 slot = BattleSide_GetModelSlot(side);
    s32 row;

    for (row = 0; row < BT4_DATA_ROWS; row++) {
        if (slot >= 0 && slot < BTL_RES_FIXED_MAX && sBt4Data[row][slot] != NULL) {
            File_Request3(Bt4Data_File(row, chara), sBt4Data[row][slot], kBt4DataSize[row]);
        }
    }
}

/* An object's lip tracks (0x1E1178, BtlLoad_StepObject). */
void Bt4Data_LoadObject(s32 id) {
    if (sBt4Data[2][BT4_DATA_OBJECT] != NULL) {
        File_Request3(Bt4Data_File(2, id), sBt4Data[2][BT4_DATA_OBJECT], kBt4DataSize[2]);
    }
}

/* Frees them (0x200880, BtlLoad_FreeAll). */
void Bt4Data_FreeAll(void) {
    s32 row;
    s32 i;

    for (row = 0; row < BT4_DATA_ROWS; row++) {
        for (i = 0; i < BTL_RES_FIXED_MAX + 1; i++) {
            if (sBt4Data[row][i] != NULL) {
                Heap_Free(sBt4Data[row][i]);
                sBt4Data[row][i] = NULL;
            }
        }
    }
}

/* Row `row` of a model slot's character, NULL when it has none (0x200790): a fixed slot's column, the first object
   slot's (in BT4 the slot numbers below 3: its two fixed slots and that one), not for a 32-byte model. */
void *Bt4Data_Get(BtlResSlot *slot, s32 row) {
    s32 n = slot->number;

    if (slot->file[0].size == 32) {
        return NULL;
    }
    if (n == BTL_RES_FIXED_MAX) {
        n = BT4_DATA_OBJECT;
    } else if (n < 0 || n > BTL_RES_FIXED_MAX) {
        return NULL;
    }
    return sBt4Data[row][n];
}

/* Entry n of a pack, NULL when it is empty (0x3BEF14). */
void *Bt4Pack_Entry(void *pack, s32 n) {
    if (BtlScene_GetPackEntrySize(pack, n) <= 0) {
        return NULL;
    }
    return BtlScene_GetPackEntry(pack, n);
}

/* ---- an object's second texture set, and a stage change's transition ---- */

/* (the decomp's names; each of its files has its own view of these structures, so they are left untyped here) */
extern void *BtlChar_FindByObjId(s32 id);
extern void *BtlChar_Get(s32 i);
extern s32 BtlAct_GetCurrentClass(void *chr);
extern void *BtlChar_GetObj(void *chr);
extern void *BtlMember_GetGauge(void *chr, s32 member);
extern void *gBtlChars;
extern void *gBtlStage;
#define BT4_AT(p, ofs, type) (*(type *)((u8 *)(p) + (ofs)))

/* Which of a fighter object's two texture sets it is drawn with (0x3BF8AC, from BtlObj_Setup): the second one
   (BtlObj_BindTables: model pack entry 26) when the member's gauge block has bit 1 of its flags. The member is the
   object's character's current one; for an object that is no character's, the member that the fighter of the
   battle's first character entry is switching to (its class below 2: its fusion partner; else only when its
   current object's class bit 30 is set). */
void Bt4Obj_PickTexture(s32 id, BtlObj *obj) {
    void *chr = BtlChar_FindByObjId(id);
    s32 member;
    s32 cls;

    if (chr != NULL) {
        member = BT4_AT(chr, 0xCD8, s32); /* (nextMember) */
    } else {
        void *first;

        if (gBtlChars == NULL || (first = BT4_AT(gBtlChars, 0x258, void *)) == NULL) {
            return;
        }
        chr = BtlChar_Get(*(s32 *)first);
        cls = BtlAct_GetCurrentClass(chr);
        if (cls < 2) {
            member = BT4_AT(chr, 0x12F8, s32); /* (fusionMember) */
            if (member < 0) {
                return;
            }
        } else {
            void *o = BtlChar_GetObj(chr);

            if (o == NULL || !(BT4_AT(BT4_AT(o, 0x92C, void *), (cls - 2) * 4 + 0xC, u32) & 0x40000000)) {
                return;
            }
            member = BT4_AT(chr, 0xCD8, s32);
        }
    }
    if (!(BT4_AT(BtlMember_GetGauge(chr, member), 0x24, u32) & 2)) {
        return;
    }
    if (BT4_AT(obj, 0x20, void *) != NULL) { /* (mdl.tex and the word after it, from the second set) */
        BT4_AT(obj, 0x48, void *) = BT4_AT(obj, 0x20, void *);
        BT4_AT(obj, 0x4C, void *) = BT4_AT(obj, 0x24, void *);
    }
}

/* A stage change's transition scene (0x240940): file 0x1BB plus a signed byte of the stage's data. */
s32 Bt4Stage_TransitionFile(void) {
    void *info;

    if (gBtlStage == NULL || (info = BT4_AT(gBtlStage, 4, void *)) == NULL) {
        return 0x1BB;
    }
    return 0x1BB + BT4_AT(BT4_AT(info, 0x14, void *), 0xC, s8);
}

/* Whether the stage has no rain for the storm effect (0x3BE9A8, EftStorm_Init): bit 0 of its data's word 1. */
s32 Bt4Stage_NoRain(void) {
    return BT4_AT(BT4_AT(BT4_AT(gBtlStage, 4, void *), 0x14, void *), 4, u32) & 1;
}

/* ---- fighting: transformation cost, the CPU's transformation, the taunt, damaged clothes ---- */

extern s32 BtlChar_TestFlag(void *chr, s32 bit);
extern void BtlChar_SetFlag(void *chr, s32 bit);
extern s32 BtlMember_DrainKi(void *chr, s32 amount);
extern void BtlMember_SubMaxPower(void *chr, s32 amount);
extern void BtlMember_AddBlast(void *chr, s32 amount);
extern void *BtlMember_GetActive(void *chr);
extern void *BtlMember_GetActiveGauge(void *chr);
extern s32 BtlUtil_Clamp(s32 v, s32 lo, s32 hi);
extern s32 BtlParam_GetSlotId(void *chr, s32 slot);
extern s32 BtlParam_GetSlotA0(void *chr, s32 slot);
extern void BtlAct_QueueTransform(void *chr, s32 slot);
extern void BtlAct_Request(void *chr, s32 id);
extern s32 BtlAct_GetQueued(void *chr);

/* What a transformation (or the taunt) takes (0x3BFAB8): ki, or with flag 6 a part of the max power gauge. */
void Bt4Act_TransformCost(void *chr, s32 cost) {
    if (BtlChar_TestFlag(chr, 6)) {
        BtlMember_SubMaxPower(chr, cost / 2 - cost / 5);
    } else {
        BtlMember_DrainKi(chr, cost);
    }
}

/* Out of ki, the CPU takes its third slot's transformation when it has one (0x3BFA20, BtlAct_UpdateGauges). */
void Bt4Act_CpuTransform(void *chr) {
    if (BtlParam_GetSlotId(chr, 3) != 0xFF && BtlParam_GetSlotA0(chr, 3) == 1) {
        BtlAct_QueueTransform(chr, 3);
        BtlAct_Request(chr, BtlAct_GetQueued(chr));
    }
}

/* After BtlAct_CheckForced's evasion (0x19BEDC): with the first fighter at that state's third step, both fighters'
   flag 98. */
void Bt4Act_ForcedEvasion(void *chr) {
    s32 i;

    if (BtlChar_Get(0) != chr || BT4_AT(chr, 0xD88, s32) < 3) {
        return;
    }
    for (i = 0; i < 2; i++) {
        BtlChar_SetFlag(BtlChar_Get(i), 98);
    }
}

/* The taunt's start (0x19BE8C, action 39): the member's taunt count, kept within -20..80. */
void Bt4Act_TauntCount(void *chr) {
    void *m = BtlMember_GetActive(chr);

    BT4_AT(m, 0x14, s32) = BtlUtil_Clamp(BT4_AT(m, 0x14, s32) + 1, -20, 80);
    BT4_AT(m, 0x4C, s32) = BT4_AT(m, 0x50, s32);
}

/* The taunt (0x19BFA0, action 43): blast for the taunter and ki off the other fighter, less the more it is used. */
void Bt4Act_Taunt(void *chr) {
    s32 n = BT4_AT(BtlMember_GetActive(chr), 0xA0, s32);
    void *other;

    /* (the PS2 divides by zero without a trap; never zero once a taunt has started) */
    BtlMember_AddBlast(chr, n != 0 ? 50000 / n : 0);
    other = BtlChar_Get(BT4_AT(chr, 0, s32) ^ 1);
    if (BtlChar_TestFlag(other, 0xBE)) {
        return;
    }
    if (!BtlChar_TestFlag(other, 6) && BT4_AT(BtlMember_GetActiveGauge(other), 0x1C, s32) != 0) {
        return;
    }
    Bt4Act_TransformCost(other, n != 0 ? 40000 / n : 0);
}

/* Hit by a blast (0x3BEE9C): a fighter with a second texture set and a gauge state of 1 or 2 is drawn with it
   from then on (its clothes torn). */
void Bt4Obj_HitTexture(void *chr) {
    void *obj = BtlChar_GetObj(chr);
    void *g;
    u32 st;

    if (BT4_AT(obj, 0x20, void *) == NULL) {
        return;
    }
    g = BtlMember_GetActiveGauge(chr);
    st = BT4_AT(g, 0x24, u32);
    if (st == 0 || st == 3) {
        return;
    }
    BT4_AT(g, 0x24, u32) = st | 2;
    BT4_AT(obj, 0x48, void *) = BT4_AT(obj, 0x20, void *);
    BT4_AT(obj, 0x4C, void *) = BT4_AT(obj, 0x24, void *);
}

/* ---- the models' file ids: from 0x1612 each character has its costumes' models, then as many damaged ones ---- */

#define BT4_CHARA_FILE_FIRST 0x1612 /* (0x20081C) */
#define BT4_CHARA_COUNT 250

/* A character's costumes: a halfword of its record in common file 4 (0x25D468). */
s32 Bt4Chr_Costumes(s32 chara) {
    u8 *table = PACK_AT(gCommonRes->data[2], 1);

    return *(u16 *)(table + chara * 0x3C + 0x28);
}

/* The first model file of a character (0x26BD98). */
static s32 Bt4Chr_First(s32 chara) {
    s32 n = 0;
    s32 i;

    for (i = 0; i < chara; i++) {
        n += Bt4Chr_Costumes(i) * 2;
    }
    return BT4_CHARA_FILE_FIRST + n;
}

/* The model of a character's costume, or its damaged version (0x26BDFC). */
s32 Bt4Chr_ModelFile(s32 chara, s32 costume, s32 damaged) {
    return Bt4Chr_First(chara) + (damaged ? Bt4Chr_Costumes(chara) : 0) + costume;
}

/* Whose model a file is, -1 for none (0x26BE6C). */
s32 Bt4Chr_OfModelFile(s32 id) {
    s32 chara;
    s32 first;

    if (id < BT4_CHARA_FILE_FIRST) {
        return -1;
    }
    for (chara = 0; chara < BT4_CHARA_COUNT; chara++) {
        first = Bt4Chr_First(chara);
        if (id == first) {
            return chara;
        }
        if (first < id && id < first + Bt4Chr_Costumes(chara) * 2) {
            return chara;
        }
    }
    return -1;
}
#endif
