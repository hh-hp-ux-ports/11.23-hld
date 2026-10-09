/*
 * kmod_fp.h -- the floating-point registers a stretch of IA-64 code names,
 * for checking the REDUCEDFP promise of a kernel object (kmod_fp.c).
 */
#ifndef HLD_KMOD_FP_H
#define HLD_KMOD_FP_H

#include "elf64.h"

/* f0, f1 and f6-f11: what the reduced floating-point model allows. */
int hld_kmod_fp_allowed(unsigned fr);

/*
 * Does slot S (0-2) of the 16-byte bundle B name an FR outside that set?
 * If so, which one goes to *bad_fr.
 */
int hld_kmod_fp_slot(const uint8_t *b, unsigned s, unsigned *bad_fr);

/*
 * Count the instructions in CODE (SIZE bytes of 16-byte bundles) that name
 * a floating-point register outside that set. The first one's offset --
 * bundle offset plus slot, as a relocation's r_offset spells it -- and
 * register go to *first_off / *first_fr when there is one.
 */
unsigned long hld_kmod_fp_scan(const uint8_t *code, uint64_t size,
                               uint64_t *first_off, unsigned *first_fr);

#endif
