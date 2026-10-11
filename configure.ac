dnl configure.ac -- hld's build configuration. Autoconf only, no automake.
dnl
dnl It finds a C compiler and the flags THAT compiler takes, and the tools the
dnl test suites need, and writes Makefile (from Makefile.in) and
dnl tests/config.sh. The Makefile itself stays plain POSIX make.
dnl
dnl The generated `configure' is committed and shipped: the HP-UX systems hld
dnl is built on have no autoconf, and only ever run the script. Regenerate it
dnl after editing this file:
dnl
dnl     autoconf
dnl
dnl and commit both. Keep their timestamps in order when copying a tree
dnl (tar, not cp -r): nothing here re-runs autoconf, but people expect a
dnl configure older than configure.ac to be stale.

AC_PREREQ([2.69])
AC_INIT([hld],
        m4_esyscmd_s([sed -n 's/^#define HLD_VERSION "\(.*\)"/\1/p' src/version.h]),
        [], [hld])
AC_CONFIG_SRCDIR([src/hld.c])

hld_os=`(uname -s) 2>/dev/null`

dnl Whether the user chose optimisation flags; autoconf fills CFLAGS in
dnl otherwise, with values meant for gcc.
hld_user_cflags=${CFLAGS+set}

AC_PROG_CC

dnl --- the flags the chosen compiler takes ---------------------------------
dnl gcc gets the warning set the code is kept clean against. HP's compilers
dnl take none of gcc's flags; they get ANSI mode -- unless autoconf already
dnl chose a language mode for them -- and an optimisation level. On HP-UX
dnl hld is built LP64 with either: gcc's default there is ILP32 (ELF32).
AS_IF([test "x$GCC" = xyes], [
    CSTD=-std=c99
    CWARN="-Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes"
    COPT="-O2 -g"
    AS_IF([test "x$hld_os" = xHP-UX], [CC="$CC -mlp64"])
], [
    CSTD=
    CWARN=
    COPT=-O
    AS_IF([test "x$hld_os" = xHP-UX], [
        AS_CASE([" $CC "], [*" -A"*], [], [CC="$CC -Ae"])
        CC="$CC +DD64"
    ])
])
AS_IF([test "x$hld_user_cflags" = xset], [COPT=$CFLAGS])

AC_MSG_CHECKING([whether $CC $CSTD $COPT builds a 64-bit program])
hld_save_CFLAGS=$CFLAGS
CFLAGS="$CSTD $COPT"
AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <stdlib.h>]],
                                [[return sizeof (void *) == 8 ? 0 : 1;]])],
    [AC_MSG_RESULT([yes])],
    [AC_MSG_RESULT([no])
     AC_MSG_ERROR([$CC cannot build hld; set CC (and CFLAGS) for this host])])
CFLAGS=$hld_save_CFLAGS

dnl --- tools the test suites use -------------------------------------------
dnl None is needed to BUILD hld; each suite skips what it cannot run. Found
dnl here once, rather than guessed at in each suite: the same tools live at
dnl different paths on 11.23 and on the 11.31 build image, and a guess that
dnl misses makes a suite skip silently.

AC_ARG_WITH([ia64-as],
    [AS_HELP_STRING([--with-ia64-as=PROG],
        [GNU as for ia64-hp-hpux, used by the link suites])],
    [XAS=$withval], [XAS=])
AC_ARG_WITH([ia64-ar],
    [AS_HELP_STRING([--with-ia64-ar=PROG], [ar that writes ia64 archives])],
    [XAR=$withval], [XAR=])
AC_ARG_WITH([gcc474],
    [AS_HELP_STRING([--with-gcc474=DIR],
        [gcc 4.7.4 install prefix, used by the dynamic suite on HP-UX])],
    [GCC474=$withval], [GCC474=])
AC_ARG_WITH([acc],
    [AS_HELP_STRING([--with-acc=PROG], [HP aC++ driver, for the vendor-compiler tests])],
    [ACC=$withval], [ACC=])

dnl GNU binutils for ia64-hp-hpux. A cross toolchain carries the target
dnl prefix; natively it is plain `as' in binutils' own prefix. HP's own
dnl /usr/ccs/bin/as speaks a different syntax and is never taken.
AC_MSG_CHECKING([for an ia64 GNU assembler])
AS_IF([test -z "$XAS"], [
    for hld_p in `echo "$PATH" | tr ':' ' '`; do
        AS_IF([test -x "$hld_p/ia64-hp-hpux11.23-as"],
              [XAS=$hld_p/ia64-hp-hpux11.23-as; break])
    done
])
AS_IF([test -z "$XAS" && test "x$hld_os" = xHP-UX], [
    for hld_p in /opt/binutils/bin /opt/gnu/binutils2461/bin; do
        AS_IF([test -x "$hld_p/as"], [XAS=$hld_p/as; break])
    done
])
AC_MSG_RESULT([${XAS:-none (the link suites will skip)}])

AC_MSG_CHECKING([for an ia64 ar])
AS_IF([test -z "$XAR"], [
    for hld_p in `echo "$PATH" | tr ':' ' '`; do
        AS_IF([test -x "$hld_p/ia64-hp-hpux11.23-ar"],
              [XAR=$hld_p/ia64-hp-hpux11.23-ar; break])
    done
])
AS_IF([test -z "$XAR" && test "x$hld_os" = xHP-UX], [
    for hld_p in /opt/binutils/bin /opt/gnu/binutils2461/bin /usr/ccs/bin; do
        AS_IF([test -x "$hld_p/ar"], [XAR=$hld_p/ar; break])
    done
])
AC_MSG_RESULT([${XAR:-none}])

AC_MSG_CHECKING([for gcc 4.7.4])
AS_IF([test -z "$GCC474" && test "x$hld_os" = xHP-UX], [
    for hld_p in /opt/gcc474 /opt/gnu/gcc474; do
        AS_IF([test -x "$hld_p/bin/gcc"], [GCC474=$hld_p; break])
    done
])
AC_MSG_RESULT([${GCC474:-none}])

AC_MSG_CHECKING([for HP aC++])
AS_IF([test -z "$ACC" && test -x /opt/aCC/bin/aCC], [ACC=/opt/aCC/bin/aCC])
AC_MSG_RESULT([${ACC:-none}])

AC_SUBST([CSTD])
AC_SUBST([CWARN])
AC_SUBST([COPT])
AC_SUBST([XAS])
AC_SUBST([XAR])
AC_SUBST([GCC474])
AC_SUBST([ACC])

AC_CONFIG_FILES([Makefile tests/config.sh])
AC_OUTPUT

AC_MSG_NOTICE([hld AC_PACKAGE_VERSION: CC='$CC' CSTD='$CSTD' CWARN='$CWARN' COPT='$COPT'])
