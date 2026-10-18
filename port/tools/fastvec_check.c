/*
 * Compares Port_FastMtxApply and Port_FastMtxSeq (port/src/plat_fastvec.c) with the long way (softfloat_ps2_inl.h),
 * bit for bit (Port_FastMtxSeq: its accumulator and the flags of its steps too).
 *     cc -O2 -Iport/src port/tools/fastvec_check.c port/src/plat_fastvec.c -o /tmp/fastvec_check && /tmp/fastvec_check [millions]
 * Prints how often the quick way gave up, too: wherever it answers, the answer must be the long way's.
 */
#include <stdio.h>
#include <string.h>
#include "softfloat_ps2_inl.h"

extern int Port_FastMtxApply(const uint32_t *r, const uint32_t *v, uint32_t *out);
extern int Port_FastMtxSeq(const uint32_t *r, const uint32_t *v, int n, uint32_t *acc, uint32_t *out, unsigned *any, unsigned *last);
extern uint32_t Port_FastAdd(uint32_t a, uint32_t b);
extern uint32_t Port_FastMul(uint32_t a, uint32_t b);

static uint64_t sState = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) {
    sState ^= sState << 13; sState ^= sState >> 7; sState ^= sState << 17;
    return (uint32_t)(sState >> 16);
}
static const uint32_t kEdge[] = {
    0x00000000, 0x80000000, 0x00000001, 0x007FFFFF, 0x00800000, 0x00800001, 0x3F800000, 0xBF800000, 0x3F7FFFFF,
    0x3F800001, 0x7F7FFFFF, 0x7F800000, 0x7FC00000, 0xFF7FFFFF, 0x80800000, 0x1F800000, 0x5F800000, 0x00FFFFFF,
};
/* kind 0: any bits; 1: exponents near 1.0 (what the game mostly has); 2: with edges and zeros mixed in */
static uint32_t operand(int kind) {
    uint32_t u = rnd();
    if (kind == 1) { return (u & 0x807FFFFF) | (uint32_t)(127 - 20 + rnd() % 41) << 23; }
    if (kind == 2) { uint32_t k = rnd() % 4; return k == 0 ? kEdge[rnd() % (sizeof(kEdge) / 4)] : k == 1 ? (u & SIGN) : (u & 0x807FFFFF) | (uint32_t)(127 - 30 + rnd() % 61) << 23; }
    if (kind == 3) { return (u & 0x80700000) | (uint32_t)(127 - 3 + rnd() % 7) << 23; } /* few bits: sums cancel */
    return u;
}
/* also the accumulator after the third step and the flags as src/port/vu0_a.c's setFlags makes them */
static void long_way(const uint32_t *r, const uint32_t *v, uint32_t *o, uint32_t *acc, unsigned *any, unsigned *last) {
    uint32_t s[4][4];
    int c, k;
    for (c = 0; c < 4; c++) {
        s[0][c] = Sf_MulBits(r[c], v[0]);
        s[1][c] = Sf_AddBits(s[0][c], Sf_MulBits(r[4 + c], v[1]));
        s[2][c] = Sf_AddBits(s[1][c], Sf_MulBits(r[8 + c], v[2]));
        s[3][c] = o[c] = Sf_AddBits(s[2][c], Sf_MulBits(r[12 + c], v[3]));
        acc[c] = s[2][c];
    }
    *any = 0;
    for (k = 0; k < 4; k++) {
        *last = 0;
        for (c = 0; c < 4; c++) { *last |= ((s[k][c] & 0x7FFFFFFF) == 0) | ((s[k][c] & SIGN) ? 2 : 0); }
        *any |= *last;
    }
}
int main(int argc, char **argv) {
    unsigned long long n = (argc > 1 ? strtoull(argv[1], NULL, 10) : 50) * 1000000ull, i, bad = 0, gaveUp[4] = {0, 0, 0, 0}, per[4] = {0, 0, 0, 0};
    for (i = 0; i < n; i++) {
        uint32_t r[16], v[4], a[4], b[4];
        int kind = (int)(i & 3), k;
        for (k = 0; k < 16; k++) { r[k] = operand(kind); }
        for (k = 0; k < 4; k++) { v[k] = operand(kind); }
        for (k = 0; k < 16; k++) { /* the single operations, on the same operands and on close pairs */
            uint32_t x = r[k], y = k < 8 ? r[15 - k] : (x ^ (rnd() & SIGN)) + (rnd() % 17) - 8 + ((rnd() % 5) << 23);
            if (Port_FastAdd(x, y) != Sf_AddBits(x, y) && bad++ < 10) { printf("add %08x %08x: quick %08x long %08x\n", x, y, Port_FastAdd(x, y), Sf_AddBits(x, y)); }
            if (Port_FastMul(x, y) != Sf_MulBits(x, y) && bad++ < 10) { printf("mul %08x %08x: quick %08x long %08x\n", x, y, Port_FastMul(x, y), Sf_MulBits(x, y)); }
        }
        /* the sequence on four vectors (Ref_Mtx_Mul): v and r's rows 1-3; then on v alone (Ref_Mtx_TranslateLocal) */
        unsigned any = 0, last = 0, one, oneLast = 0, qany = 0, qlast = 9, up = 0;
        uint32_t vs[16], a4[16], acc[4], acc1[4], qacc[4], q[16];
        for (k = 0; k < 4; k++) { vs[k] = v[k]; vs[4 + k] = r[4 + k]; vs[8 + k] = r[8 + k]; vs[12 + k] = r[12 + k]; }
        for (k = 0; k < 4; k++) {
            unsigned f, fl;
            long_way(r, vs + 4 * k, a4 + 4 * k, acc, &fl, &f);
            any |= fl;
            last = f;
            if (k == 0) { one = fl; oneLast = f; memcpy(acc1, acc, sizeof(acc)); }
            up |= !Port_FastMtxApply(r, vs + 4 * k, b);
        }
        if (Port_FastMtxSeq(r, vs, 4, qacc, q, &qany, &qlast) ? up || memcmp(a4, q, sizeof(a4)) != 0 || memcmp(acc, qacc, sizeof(acc)) != 0 || qany != any || qlast != last : !up) {
            if (bad++ < 10) { printf("seq of 4 differs (kind %d, apply gave up %u): acc %08x/%08x flags %x/%x %x/%x\n", kind, up, acc[0], qacc[0], any, qany, last, qlast); }
        }
        memcpy(a, a4, sizeof(a));
        per[kind]++;
        qany = 0;
        if (!Port_FastMtxApply(r, v, b)) {
            gaveUp[kind]++;
            if (Port_FastMtxSeq(r, v, 1, qacc, q, &qany, &qlast) && bad++ < 10) { printf("seq answered where apply gave up\n"); }
            continue;
        }
        if ((!Port_FastMtxSeq(r, v, 1, qacc, q, &qany, &qlast) || memcmp(a, q, sizeof(a)) != 0 || memcmp(acc1, qacc, sizeof(acc1)) != 0 ||
             qany != one || qlast != oneLast) && bad++ < 10) {
            printf("seq differs (kind %d): acc %08x/%08x out %08x/%08x flags %x/%x %x/%x\n", kind, acc1[0], qacc[0], a[0], q[0], one, qany, oneLast, qlast);
        }
        if (memcmp(a, b, sizeof(a)) != 0 && bad++ < 10) {
            printf("differs (kind %d): long %08x %08x %08x %08x quick %08x %08x %08x %08x\n", kind, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
    }
    printf("%llu million matrices, %llu differences; gave up: any bits %.1f%%, near 1.0 %.2f%%, edges %.1f%%, few bits %.2f%%\n", n / 1000000ull, bad,
           100.0 * gaveUp[0] / per[0], 100.0 * gaveUp[1] / per[1], 100.0 * gaveUp[2] / per[2], 100.0 * gaveUp[3] / per[3]);
    return bad != 0;
}
