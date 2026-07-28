/* ia64_patch.h — IA-64 instruction bundle / data field insertion for hld.
   See ia64_patch.c for provenance (GNU binutils) and license (GPLv3+).  */
#ifndef HLD_IA64_PATCH_H
#define HLD_IA64_PATCH_H

#include <stdint.h>

typedef enum {
    HLD_PATCH_OK = 0,
    HLD_PATCH_OVERFLOW,     /* value does not fit the instruction field */
    HLD_PATCH_UNSUPPORTED,  /* relocation is not a field this function installs */
    HLD_PATCH_BADSLOT       /* slot is invalid for this relocation's format */
} hld_patch_status;

/*
 * Install VAL into the field selected by R_TYPE.
 *
 * Instruction relocations: HIT is the 16-byte bundle base, SLOT is 0..2
 * (the psABI encodes the slot in r_offset's low 2 bits; callers pass it
 * explicitly — offset&~3 addresses the bundle, offset&3 is the slot).
 * IMM64-class and PCREL60B relocations occupy the L+X slots and require
 * SLOT == 1.
 *
 * Data relocations (DIR32/64, SEGREL, FPTR64MSB, ...): HIT is the target
 * byte address, SLOT is ignored.
 *
 * R_IA64_NONE and R_IA64_LDXMOV install nothing and return OK (LDXMOV is
 * the relaxation marker; hld's non-relaxing policy leaves the ld8 as-is).
 *
 * NOTE (inherited GNU behavior): instruction fields are OR-merged, not
 * cleared first — correct for assembler output, where pending-relocation
 * fields are zero. Do not patch the same field twice. The IMM64/PCREL60B
 * paths do clear their fields and are re-patchable.
 */
int hld_ia64_reloc_is_insn(uint32_t r_type);

hld_patch_status hld_ia64_install_value(uint8_t *hit, unsigned slot,
                                        uint64_t val, uint32_t r_type);

#endif /* HLD_IA64_PATCH_H */
