/*
 * version.h — hld's version, in one place.
 *
 * This is the single source of truth: the linker reports it for -V, stamps
 * it into everything it links, and scripts/mkdepot.sh reads it from here so
 * a depot cannot claim a version the binary inside it does not.
 *
 * The stamp is a `what` string — the platform's own convention, marked by
 * the `@(#)` prefix, so `what <file>` reports it for the linker itself and
 * for anything the linker produced. Knowing which linker built a binary is
 * not a nicety: when a link is suspected of miscompiling something, the
 * first question is which version produced it, and file timestamps are a
 * poor answer.
 */
#ifndef HLD_VERSION_H
#define HLD_VERSION_H

#define HLD_VERSION "0.10.0"

#define HLD_IDENT "@(#)hld " HLD_VERSION " - LP64 linker for HP-UX 11.23/IPF"

/*
 * What -V and --version print after the version line. The bundle patcher is
 * derived from GNU binutils (src/ia64_patch.c), which is why the licence is
 * GPLv3+ and why binutils is named here rather than only in the README.
 */
#define HLD_COPYRIGHT \
    "Copyright (C) 2026 Hugo Hurskainen\n" \
    "License GPLv3+: GNU GPL version 3 or later " \
    "<https://gnu.org/licenses/gpl.html>\n" \
    "This is free software: you are free to change and redistribute it.\n" \
    "There is NO WARRANTY, to the extent permitted by law.\n" \
    "Instruction-bundle handling is derived from GNU binutils."

#endif
