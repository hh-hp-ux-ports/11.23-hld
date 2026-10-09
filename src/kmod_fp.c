/*
 * kmod_fp.c -- which floating-point registers a stretch of IA-64 code names.
 *
 * A kernel object built for HP-UX 11.31 claims EF_IA_64_REDUCEDFP: its code
 * touches only f0/f1 (the constants) and f6-f11, so the kernel need not save
 * the rest of the floating-point file around it. The claim is a promise to
 * the kernel, and a wrong one corrupts some user process's floating point
 * long after the module ran. When hld sets that bit on objects whose
 * compiler cannot (gcc), it checks the promise here, instruction by
 * instruction, rather than taking it on trust.
 *
 * Written from the instruction formats in the Itanium architecture manual
 * (vol. 3, ch. 4). Only the formats that carry a floating-point register
 * field are decoded:
 *
 *   F unit    every format; the f1..f4 fields where the format has them
 *   M unit    ldf/ldfp/stf (M6-M13), setf (M18), getf (M19), chk.s.m with
 *             an FR (M21)
 *
 * A field a format does not have reads as whatever bits sit there, so the
 * register fields are taken per format, never blindly. Checked against the
 * 36 modules in 11.23's /usr/conf/mod, all marked REDUCEDFP (no finding),
 * and against gcc output compiled with and without -mfixed-range (the
 * f12-f15 and f32+ it allocates without it are all found); see
 * docs/kernel-modules.md.
 */
#include "port.h"

#include <string.h>

#include "kmod_fp.h"

/* Slot unit types, per template. 0 marks a reserved template. */
enum { U_NONE, U_M, U_I, U_F, U_B, U_L };

static const unsigned char tmpl_units[32][3] = {
    { U_M, U_I, U_I }, { U_M, U_I, U_I }, { U_M, U_I, U_I }, { U_M, U_I, U_I },
    { U_M, U_L, U_L }, { U_M, U_L, U_L }, { U_NONE }, { U_NONE },
    { U_M, U_M, U_I }, { U_M, U_M, U_I }, { U_M, U_M, U_I }, { U_M, U_M, U_I },
    { U_M, U_F, U_I }, { U_M, U_F, U_I }, { U_M, U_M, U_F }, { U_M, U_M, U_F },
    { U_M, U_I, U_B }, { U_M, U_I, U_B }, { U_M, U_B, U_B }, { U_M, U_B, U_B },
    { U_NONE }, { U_NONE }, { U_B, U_B, U_B }, { U_B, U_B, U_B },
    { U_M, U_M, U_B }, { U_M, U_M, U_B }, { U_NONE }, { U_NONE },
    { U_M, U_F, U_B }, { U_M, U_F, U_B }, { U_NONE }, { U_NONE },
};

static uint64_t bits(uint64_t insn, unsigned lo, unsigned n)
{
    return (insn >> lo) & ((1ULL << n) - 1);
}

/* f0 and f1 are constants; f6-f11 are the scratch registers the model keeps. */
int hld_kmod_fp_allowed(unsigned fr)
{
    return fr <= 1 || (fr >= 6 && fr <= 11);
}

/* The FR fields of one instruction: up to four register numbers. */
static unsigned fr_fields(unsigned unit, uint64_t in, unsigned fr[4])
{
    unsigned op = (unsigned)bits(in, 37, 4);
    unsigned n = 0;
    unsigned f1 = (unsigned)bits(in, 6, 7), f2 = (unsigned)bits(in, 13, 7);
    unsigned f3 = (unsigned)bits(in, 20, 7), f4 = (unsigned)bits(in, 27, 7);

    if (unit == U_F) {
        switch (op) {
        case 0x0: case 0x1:
            if (bits(in, 33, 1)) {          /* F6/F7: frcpa, frsqrta (+fp) */
                fr[n++] = f1; fr[n++] = f2; fr[n++] = f3;
            } else {
                unsigned x6 = (unsigned)bits(in, 27, 6);
                /* F15/F14/F12/F13: break.f, nop.f/hint.f, fsetc, fclrf, fchkf */
                if (x6 == 0x00 || x6 == 0x01 || x6 == 0x04 || x6 == 0x05
                    || x6 == 0x08)
                    break;
                /* F8-F11: fmin..fmerge..fcvt; an absent f3 reads as f0 */
                fr[n++] = f1; fr[n++] = f2; fr[n++] = f3;
            }
            break;
        case 0x4:                           /* F4: fcmp -- p1, p2 elsewhere */
            fr[n++] = f2; fr[n++] = f3;
            break;
        case 0x5:                           /* F5: fclass */
            fr[n++] = f2;
            break;
        case 0x8: case 0x9: case 0xa: case 0xb:
        case 0xc: case 0xd: case 0xe:       /* F1-F3: fma..fnma, fselect, xma */
            fr[n++] = f1; fr[n++] = f2; fr[n++] = f3; fr[n++] = f4;
            break;
        default:
            break;
        }
        return n;
    }
    if (unit == U_M) {
        unsigned m = (unsigned)bits(in, 36, 1), x = (unsigned)bits(in, 27, 1);
        unsigned x6 = (unsigned)bits(in, 30, 6);
        int is_load = x6 <= 0x1b || (x6 >= 0x20 && x6 <= 0x27);
        int is_store = x6 >= 0x30 && x6 <= 0x3b;

        switch (op) {
        case 0x6:
            if (!x) {
                if (is_load) fr[n++] = f1;             /* M6/M7 ldf */
                else if (is_store && !m) fr[n++] = f2; /* M9 stf */
                /* 0x2c-0x2f lfetch: no FR */
            } else if (!m && x6 >= 0x1c && x6 <= 0x1f) {
                fr[n++] = f1;                           /* M18 setf */
            } else if (is_load) {
                fr[n++] = f1; fr[n++] = f2;             /* M11/M12 ldfp */
            }
            break;
        case 0x7:
            if (is_load) fr[n++] = f1;                  /* M8 ldf, imm update */
            else if (is_store) fr[n++] = f2;            /* M10 stf, imm update */
            break;
        case 0x4:
            if (x && !m && x6 >= 0x1c && x6 <= 0x1f)
                fr[n++] = f2;                           /* M19 getf */
            break;
        case 0x1:
            if (bits(in, 33, 3) == 3) fr[n++] = f2;     /* M21 chk.s.m fR */
            break;
        default:
            break;
        }
    }
    return n;
}

int hld_kmod_fp_slot(const uint8_t *b, unsigned s, unsigned *bad_fr)
{
    uint64_t lo = 0, hi = 0, insn;
    unsigned t, fr[4], n, i, unit;
    int k;

    for (k = 7; k >= 0; k--) lo = (lo << 8) | b[k];
    for (k = 15; k >= 8; k--) hi = (hi << 8) | b[k];
    t = (unsigned)(lo & 0x1f);
    unit = tmpl_units[t][s];
    if (unit != U_M && unit != U_F) return 0;
    if (s == 0) insn = lo >> 5;
    else if (s == 1) insn = (lo >> 46) | (hi << 18);
    else insn = hi >> 23;
    insn &= (1ULL << 41) - 1;
    n = fr_fields(unit, insn, fr);
    for (i = 0; i < n; i++) {
        if (hld_kmod_fp_allowed(fr[i])) continue;
        if (bad_fr) *bad_fr = fr[i];
        return 1;
    }
    return 0;
}

unsigned long hld_kmod_fp_scan(const uint8_t *code, uint64_t size,
                               uint64_t *first_off, unsigned *first_fr)
{
    uint64_t off;
    unsigned long bad = 0;
    unsigned s, fr;

    for (off = 0; off + 16 <= size; off += 16)
        for (s = 0; s < 3; s++) {
            if (!hld_kmod_fp_slot(code + off, s, &fr)) continue;
            if (!bad++) {
                if (first_off) *first_off = off + s;
                if (first_fr) *first_fr = fr;
            }
        }
    return bad;
}
