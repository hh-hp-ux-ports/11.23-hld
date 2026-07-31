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

#define HLD_VERSION "0.9.2"

#define HLD_IDENT "@(#)hld " HLD_VERSION " - LP64 linker for HP-UX 11.23/IPF"

#endif
