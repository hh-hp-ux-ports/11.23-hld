/*
 * port.h — must be the FIRST include of every hld .c file (feature-test
 * macros have to precede all system headers).
 *
 * HP-UX 11.23: strict -std=c99 (__STRICT_ANSI__) hides snprintf & friends in
 * <stdio.h>. Probed on real hardware (gcc 4.7.4 -mlp64 -std=c99): _HPUX_SOURCE
 * is the one macro that exposes them; _XOPEN_SOURCE=600 /
 * _XOPEN_SOURCE_EXTENDED / _INCLUDE__STDC_A1_SOURCE all do not.
 */
#ifndef HLD_PORT_H
#define HLD_PORT_H

#ifdef __hpux
#ifndef _HPUX_SOURCE
#define _HPUX_SOURCE 1
#endif
#endif

#endif /* HLD_PORT_H */
