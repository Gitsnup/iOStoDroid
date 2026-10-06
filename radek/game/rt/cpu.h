/* Translated-game CPU model: ARMv6 user-mode state in portable C99.
 *
 * Game pointers are 32-bit values. Data pointers index MEMBASE directly
 * (the reservation covers [0, MEM_SIZE)); code pointers are small nonzero
 * IDs resolved through the generated function table. Return continuations
 * created by the translator carry VRET_BIT and are never dispatched.
 */
#ifndef RADEK_GAME_CPU_H
#define RADEK_GAME_CPU_H

#include <stdint.h>
#include <string.h>
#include <math.h>

#define MEM_SIZE ((uint32_t)0xE0800000u)
#define VRET_BIT ((uint32_t)0x80000000u)

extern uint8_t *MEMBASE;

typedef struct CPU {
    uint32_t r[16];
    uint32_t cpsr;   /* NZCV in bits 31..28 */
    uint64_t d[16];  /* VFP double banks; s[2i]/s[2i+1] are halves of d[i] */
    uint32_t fpscr;  /* only NZCV bits are modelled (never configured) */
} CPU;

/* NZCV bits */
#define F_N ((uint32_t)1u << 31)
#define F_Z ((uint32_t)1u << 30)
#define F_C ((uint32_t)1u << 29)
#define F_V ((uint32_t)1u << 28)

/* ---- memory (unaligned-safe; ARMv6 does true unaligned loads) ---- */
static inline uint32_t rd32(uint32_t a) {
    uint32_t v;
    memcpy(&v, MEMBASE + a, 4);
    return v;
}
static inline void wr32(uint32_t a, uint32_t v) {
    memcpy(MEMBASE + a, &v, 4);
}
static inline uint32_t rd16(uint32_t a) {
    uint16_t v;
    memcpy(&v, MEMBASE + a, 2);
    return v;
}
static inline void wr16(uint32_t a, uint32_t v) {
    uint16_t w = (uint16_t)v;
    memcpy(MEMBASE + a, &w, 2);
}
static inline uint32_t rd8(uint32_t a) {
    return MEMBASE[a];
}
static inline uint64_t rd64(uint32_t a) {
    uint64_t v;
    memcpy(&v, MEMBASE + a, 8);
    return v;
}
static inline void wr64(uint32_t a, uint64_t v) {
    memcpy(MEMBASE + a, &v, 8);
}
static inline void wr8(uint32_t a, uint32_t v) {
    MEMBASE[a] = (uint8_t)v;
}

/* ---- VFP single/double views over the banks ---- */
static inline uint32_t sget(CPU *cpu, unsigned s) {
    return (uint32_t)(cpu->d[s >> 1] >> ((s & 1) * 32));
}
static inline void sset(CPU *cpu, unsigned s, uint32_t v) {
    unsigned i = s >> 1;
    if (s & 1)
        cpu->d[i] = (cpu->d[i] & 0xFFFFFFFFull) | ((uint64_t)v << 32);
    else
        cpu->d[i] = (cpu->d[i] & 0xFFFFFFFF00000000ull) | v;
}
static inline float u2f(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static inline uint32_t f2u(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}
static inline double u2d(uint64_t v) {
    double f;
    memcpy(&f, &v, 8);
    return f;
}
static inline uint64_t d2u(double f) {
    uint64_t v;
    memcpy(&v, &f, 8);
    return v;
}

/* ---- VFP compare: NZCV nibble (VCMP/VCMPE differ only in quiet-NaN
 * signalling, which no game code can observe: FPSCR is never read back
 * except through FMSTAT, and never configured). Unordered (either NaN)
 * sets C+V; equal sets Z+C; less sets N; greater sets C. ---- */
static inline uint32_t vfp_nzcv_f(float a, float b) {
    if (isnan(a) || isnan(b))
        return 0x30000000u;
    if (a == b)
        return 0x60000000u;
    return (a < b) ? 0x80000000u : 0x20000000u;
}
static inline uint32_t vfp_nzcv_d(double a, double b) {
    if (isnan(a) || isnan(b))
        return 0x30000000u;
    if (a == b)
        return 0x60000000u;
    return (a < b) ? 0x80000000u : 0x20000000u;
}

/* ---- VFP float->integer (VCVT, FPSCR default round-to-nearest-even).
 * NaN converts to 0; out-of-range values saturate. All casts below are
 * fed in-range values only, so no C undefined behaviour. ---- */
static inline uint32_t vcvt_s32_f(float x) {
    double r;
    if (isnan(x))
        return 0;
    r = rint((double)x);
    if (r >= 2147483648.0)
        return 0x7FFFFFFFu;
    if (r < -2147483648.0)
        return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
static inline uint32_t vcvt_s32_d(double x) {
    double r;
    if (isnan(x))
        return 0;
    r = rint(x);
    if (r >= 2147483648.0)
        return 0x7FFFFFFFu;
    if (r < -2147483648.0)
        return 0x80000000u;
    return (uint32_t)(int32_t)r;
}
static inline uint32_t vcvt_u32_f(float x) {
    double r;
    if (isnan(x))
        return 0;
    r = rint((double)x);
    if (r >= 4294967296.0)
        return 0xFFFFFFFFu;
    if (r <= 0.0)
        return 0;
    return (uint32_t)r;
}
static inline uint32_t vcvt_u32_d(double x) {
    double r;
    if (isnan(x))
        return 0;
    r = rint(x);
    if (r >= 4294967296.0)
        return 0xFFFFFFFFu;
    if (r <= 0.0)
        return 0;
    return (uint32_t)r;
}

/* ---- condition codes ---- */
static inline int cc_pass(uint32_t cpsr, unsigned cc) {
    unsigned n = (cpsr >> 31) & 1, z = (cpsr >> 30) & 1;
    unsigned c = (cpsr >> 29) & 1, v = (cpsr >> 28) & 1;
    switch (cc) {
    case 0: return z;            /* eq */
    case 1: return !z;           /* ne */
    case 2: return c;            /* hs/cs */
    case 3: return !c;           /* lo/cc */
    case 4: return n;            /* mi */
    case 5: return !n;           /* pl */
    case 6: return v;            /* vs */
    case 7: return !v;           /* vc */
    case 8: return c && !z;      /* hi */
    case 9: return !c || z;      /* ls */
    case 10: return n == v;      /* ge */
    case 11: return n != v;      /* lt */
    case 12: return !z && n == v;/* gt */
    case 13: return z || n != v; /* le */
    case 14: return 1;           /* al */
    default: return 0;           /* nv: never executes */
    }
}

/* ---- flag computers (result already truncated to 32 bits) ---- */
static inline uint32_t fl_nz(uint32_t res) {
    uint32_t f = 0;
    if (res >> 31) f |= F_N;
    if (!res) f |= F_Z;
    return f;
}
/* add: carry = carry-out of bit 31, overflow = signed overflow */
static inline uint32_t fl_add(uint32_t a, uint32_t b, uint32_t res) {
    uint32_t f = fl_nz(res);
    if (res < a) f |= F_C;
    if (((a ^ res) & (b ^ res)) >> 31) f |= F_V;
    return f;
}
/* sub (a - b): C = NOT borrow; V = signed overflow */
static inline uint32_t fl_sub(uint32_t a, uint32_t b, uint32_t res) {
    uint32_t f = fl_nz(res);
    if (a >= b) f |= F_C;
    if (((a ^ b) & (a ^ res)) >> 31) f |= F_V;
    return f;
}

#endif
