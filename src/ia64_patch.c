/* ia64_patch.c — IA-64 instruction bundle / data field insertion.

   Derived from GNU binutils 2.46.1:
     bfd/elfxx-ia64.c     ia64_elf_install_value
     bfd/cpu-ia64-opc.c   ins_imms_scaled and the IMM14/IMM22/TGT25* operand
                          bit-field tables
   Copyright (C) 1998-2026 Free Software Foundation, Inc.
   Contributed by David Mosberger-Tang <davidm@hpl.hp.com>.
   Adapted for hld (2026): explicit slot parameter instead of pointer
   low-bit tricks, hld types and status codes, MSB data forms via hld's
   byte accessors. Field layouts and insertion order are kept verbatim so
   the code stays diffable against upstream.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  */

#include "port.h"

#include <stddef.h>

#include "elf64.h"
#include "ia64_patch.h"

struct bitfield { int bits, shift; };

/* Operand descriptors: scale + up to 4 bit-fields, least significant
   field first (values are consumed field-by-field, LSB first), exactly as
   in cpu-ia64-opc.c's elf64_ia64_operands[].  */
struct opnd_desc { int scale; struct bitfield field[4]; };

static const struct opnd_desc opnd_imm14 =  /* IMM14: adds imm */
    { 0, { { 7, 13 }, { 6, 27 }, { 1, 36 }, { 0, 0 } } };
static const struct opnd_desc opnd_imm22 =  /* IMM22: addl imm */
    { 0, { { 7, 13 }, { 9, 27 }, { 5, 22 }, { 1, 36 } } };
static const struct opnd_desc opnd_tgt25 =  /* TGT25: fchkf */
    { 4, { { 20, 6 }, { 1, 36 }, { 0, 0 }, { 0, 0 } } };
static const struct opnd_desc opnd_tgt25b = /* TGT25b: mod-sched branches */
    { 4, { { 7, 6 }, { 13, 20 }, { 1, 36 }, { 0, 0 } } };
static const struct opnd_desc opnd_tgt25c = /* TGT25c: br / br.call */
    { 4, { { 20, 13 }, { 1, 36 }, { 0, 0 }, { 0, 0 } } };

/* The bits an operand occupies, so they can be cleared before insertion. */
static uint64_t
opnd_mask(const struct opnd_desc *self)
{
    uint64_t m = 0;
    int i;

    for (i = 0; i < 4 && self->field[i].bits; ++i)
        m |= ((((uint64_t)1 << self->field[i].bits) - 1)
              << self->field[i].shift);
    return m;
}

/* cpu-ia64-opc.c ins_imms_scaled, verbatim modulo types: scatter the
   sign-extended, scale-shifted value across the fields, LSB first; range
   check via the final sign bit.  */
static int
ins_imms_scaled(const struct opnd_desc *self, uint64_t value, uint64_t *code)
{
    int64_t svalue = (int64_t)value, sign_bit = 0;
    uint64_t new_insn = 0;
    int i;

    svalue >>= self->scale;

    for (i = 0; i < 4 && self->field[i].bits; ++i) {
        new_insn |= (((uint64_t)svalue
                      & ((((uint64_t)1) << self->field[i].bits) - 1))
                     << self->field[i].shift);
        sign_bit = (svalue >> (self->field[i].bits - 1)) & 1;
        svalue >>= self->field[i].bits;
    }
    if ((!sign_bit && svalue != 0) || (sign_bit && svalue != -1))
        return -1; /* "integer operand out of range" */

    *code |= new_insn;
    return 0;
}

hld_patch_status
hld_ia64_install_value(uint8_t *hit, unsigned slot, uint64_t v, uint32_t r_type)
{
    const struct opnd_desc *opnd = NULL;
    int longform = 0;   /* 1 = IMMU64 (movl), 2 = TGT64 (brl) */
    int size = 0, bigendian = 0, shift = 0;
    uint64_t t0, t1, dword, insn;
    uint64_t val = v;

    switch (r_type) {
    case R_IA64_NONE:
    case R_IA64_LDXMOV:
        return HLD_PATCH_OK;

        /* Instruction relocations.  */

    case R_IA64_IMM14:
    case R_IA64_TPREL14:
    case R_IA64_DTPREL14:
        opnd = &opnd_imm14;
        break;

    case R_IA64_PCREL21F:  opnd = &opnd_tgt25;  break;
    case R_IA64_PCREL21M:  opnd = &opnd_tgt25b; break;
    case R_IA64_PCREL60B:  longform = 2;        break;
    case R_IA64_PCREL21B:
    case R_IA64_PCREL21BI:
        opnd = &opnd_tgt25c;
        break;

    case R_IA64_IMM22:
    case R_IA64_GPREL22:
    case R_IA64_LTOFF22:
    case R_IA64_LTOFF22X:
    case R_IA64_PLTOFF22:
    case R_IA64_PCREL22:
    case R_IA64_LTOFF_FPTR22:
    case R_IA64_TPREL22:
    case R_IA64_DTPREL22:
    case R_IA64_LTOFF_TPREL22:
    case R_IA64_LTOFF_DTPMOD22:
    case R_IA64_LTOFF_DTPREL22:
        opnd = &opnd_imm22;
        break;

    case R_IA64_IMM64:
    case R_IA64_GPREL64I:
    case R_IA64_LTOFF64I:
    case R_IA64_PLTOFF64I:
    case R_IA64_PCREL64I:
    case R_IA64_FPTR64I:
    case R_IA64_LTOFF_FPTR64I:
    case R_IA64_TPREL64I:
    case R_IA64_DTPREL64I:
        longform = 1;
        break;

        /* Data relocations.  */

    case R_IA64_DIR32MSB:
    case R_IA64_GPREL32MSB:
    case R_IA64_FPTR32MSB:
    case R_IA64_PCREL32MSB:
    case R_IA64_LTOFF_FPTR32MSB:
    case R_IA64_SEGREL32MSB:
    case R_IA64_SECREL32MSB:
    case R_IA64_LTV32MSB:
    case R_IA64_DTPREL32MSB:
        size = 4; bigendian = 1;
        break;

    case R_IA64_DIR32LSB:
    case R_IA64_GPREL32LSB:
    case R_IA64_FPTR32LSB:
    case R_IA64_PCREL32LSB:
    case R_IA64_LTOFF_FPTR32LSB:
    case R_IA64_SEGREL32LSB:
    case R_IA64_SECREL32LSB:
    case R_IA64_LTV32LSB:
    case R_IA64_DTPREL32LSB:
        size = 4; bigendian = 0;
        break;

    case R_IA64_DIR64MSB:
    case R_IA64_GPREL64MSB:
    case R_IA64_PLTOFF64MSB:
    case R_IA64_FPTR64MSB:
    case R_IA64_PCREL64MSB:
    case R_IA64_LTOFF_FPTR64MSB:
    case R_IA64_SEGREL64MSB:
    case R_IA64_SECREL64MSB:
    case R_IA64_LTV64MSB:
    case R_IA64_TPREL64MSB:
    case R_IA64_DTPMOD64MSB:
    case R_IA64_DTPREL64MSB:
        size = 8; bigendian = 1;
        break;

    case R_IA64_DIR64LSB:
    case R_IA64_GPREL64LSB:
    case R_IA64_PLTOFF64LSB:
    case R_IA64_FPTR64LSB:
    case R_IA64_PCREL64LSB:
    case R_IA64_LTOFF_FPTR64LSB:
    case R_IA64_SEGREL64LSB:
    case R_IA64_SECREL64LSB:
    case R_IA64_LTV64LSB:
    case R_IA64_TPREL64LSB:
    case R_IA64_DTPMOD64LSB:
    case R_IA64_DTPREL64LSB:
        size = 8; bigendian = 0;
        break;

        /* Unsupported / dynamic relocations.  */
    default:
        return HLD_PATCH_UNSUPPORTED;
    }

    /* Data relocation: plain (endian-explicit) store.  */
    if (size == 4) {
        if (bigendian) st32(hit, (uint32_t)val);
        else { hit[0] = (uint8_t)val; hit[1] = (uint8_t)(val >> 8);
               hit[2] = (uint8_t)(val >> 16); hit[3] = (uint8_t)(val >> 24); }
        return HLD_PATCH_OK;
    }
    if (size == 8) {
        if (bigendian) st64(hit, val);
        else stle64(hit, val);
        return HLD_PATCH_OK;
    }

    /* Long-format instruction relocations (X-unit, L+X slots).
       tmpl/s: bits  0.. 5 in t0
       slot 0: bits  5..45 in t0
       slot 1: bits 46..63 in t0, bits 0..22 in t1
       slot 2: bits 23..63 in t1  */
    if (longform == 1) {                                /* IMMU64: movl */
        if (slot != 1)
            return HLD_PATCH_BADSLOT;
        t0 = le64(hit);
        t1 = le64(hit + 8);

        /* First, clear the bits that form the 64 bit constant.  */
        t0 &= ~(0x3ffffULL << 46);
        t1 &= ~(0x7fffffULL
                | ((  (0x07fULL << 13) | (0x1ffULL << 27)
                      | (0x01fULL << 22) | (0x001ULL << 21)
                      | (0x001ULL << 36)) << 23));

        t0 |= ((val >> 22) & 0x03ffffULL) << 46;          /* 18 lsbs of imm41 */
        t1 |= ((val >> 40) & 0x7fffffULL) <<  0;          /* 23 msbs of imm41 */
        t1 |= (  (((val >>  0) & 0x07f) << 13)            /* imm7b */
                 | (((val >>  7) & 0x1ff) << 27)          /* imm9d */
                 | (((val >> 16) & 0x01f) << 22)          /* imm5c */
                 | (((val >> 21) & 0x001) << 21)          /* ic */
                 | (((val >> 63) & 0x001) << 36)) << 23;  /* i */

        stle64(hit, t0);
        stle64(hit + 8, t1);
        return HLD_PATCH_OK;
    }
    if (longform == 2) {                                /* TGT64: brl */
        if (slot != 1)
            return HLD_PATCH_BADSLOT;
        t0 = le64(hit);
        t1 = le64(hit + 8);

        /* First, clear the bits that form the 64 bit constant.  */
        t0 &= ~(0x3ffffULL << 46);
        t1 &= ~(0x7fffffULL
                | ((1ULL << 36 | 0xfffffULL << 13) << 23));

        val >>= 4;
        t0 |= ((val >> 20) & 0xffffULL) << 2 << 46;       /* 16 lsbs of imm39 */
        t1 |= ((val >> 36) & 0x7fffffULL) << 0;           /* 23 msbs of imm39 */
        t1 |= ((((val >> 0) & 0xfffffULL) << 13)          /* imm20b */
                | (((val >> 59) & 0x1ULL) << 36)) << 23;  /* i */

        stle64(hit, t0);
        stle64(hit + 8, t1);
        return HLD_PATCH_OK;
    }

    /* Ordinary 41-bit slot: OR the operand fields into the syllable.
       Upstream's hit_addr arrives as bundle+slot (r_offset low bits) and
       then adds {0,3,6}; our hit is the bundle base, so add slot first —
       net byte offsets {0,4,8}, landing the 41-bit syllable at in-dword
       bit {5,14,23}.  */
    hit += slot;
    switch (slot) {
    case 0: shift =  5; break;
    case 1: shift = 14; hit += 3; break;
    case 2: shift = 23; hit += 6; break;
    default: return HLD_PATCH_BADSLOT;
    }
    dword = le64(hit);
    insn = (dword >> shift) & 0x1ffffffffffULL;

    /*
     * Clear the operand's bits before inserting. Upstream ORs the value in,
     * which suits an assembler that leaves a pending field zero — GNU as does,
     * but HP's assembler leaves a non-zero placeholder there, and OR-ing into
     * it produces a wrong immediate. Clearing first is correct for both.
     */
    insn &= ~opnd_mask(opnd);

    if (ins_imms_scaled(opnd, val, &insn) != 0)
        return HLD_PATCH_OVERFLOW;

    dword &= ~(0x1ffffffffffULL << shift);
    dword |= (insn << shift);
    stle64(hit, dword);
    return HLD_PATCH_OK;
}
