#!/bin/sh
# Build hld's HP-UX binaries OFF the target: a cross gcc compiles them for
# ia64-hp-hpux11.23 (LP64), and hld itself -- this tree's, built for the
# host first with ./configure && make -- links them against a copy of the
# target's libraries through --sysroot. Release depots are built this way
# (docs/packaging.md); nothing is compiled on an HP-UX system.
#
#   XGCC=... XINC=... SYSROOT=... LIBGCC=... sh scripts/xbuild.sh [OUTDIR]
#
#   XGCC     the cross C compiler, with any -B it needs, e.g.
#              "/home/claude/crossbuild/b-gcc474/gcc/xgcc -B/home/claude/crossbuild/b-gcc474/gcc/"
#   XINC     the target's header directories, searched in order. gcc's
#            fixincluded headers must come BEFORE /usr/include, or HP's
#            stdio.h fails on __va_list__ (that xgcc ignores --sysroot).
#   SYSROOT  holds usr/lib/hpux64 with the target's libc.so.1,
#            libunwind.so.1, unix98.o, libcres.a and milli.a
#   LIBGCC   gcc's hpux64 libgcc.a for the same compiler version
#   OUTDIR   where the binaries go (default build/hpux)
#
# The flags are the ones ./configure gives gcc: -std=c99, its warning set,
# -O2 -g. The link is the one gcc 4.7.4 runs on HP-UX: unix98.o first, -u
# main (the entry point), libgcc, libunwind and libc, and -z (trap NULL
# dereferences). No -L is given, so the run path records only the target's
# own /usr/lib/hpux64 and /opt/langtools/lib/hpux64, never a build directory.

set -e
: "${XGCC:?set XGCC to the cross compiler}" "${XINC:?set XINC to the header dirs}"
: "${SYSROOT:?set SYSROOT}" "${LIBGCC:?set LIBGCC to libgcc.a}"
OUT=${1:-build/hpux}
HLD=build/hld
[ -x $HLD ] || { echo "xbuild: $HLD is missing -- ./configure && make first" >&2; exit 1; }

CFLAGS="-mlp64 -std=c99 -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -O2 -g"
INC=
for d in $XINC; do INC="$INC -isystem $d"; done

READER="src/elfread.c"
LINKSRC="src/hld.c src/link.c src/write.c src/dynamic.c src/archive.c src/stubs.c src/relocatable.c src/kmod_fp.c src/ia64_patch.c $READER"

rm -rf $OUT && mkdir -p $OUT/obj

# compile each source once, then link a program from a list of them
cc1() {
    o=$OUT/obj/`echo $1 | tr / _ | sed 's/\.c$/.o/'`
    [ -f $o ] || $XGCC $CFLAGS $INC -c $1 -o $o
    echo $o
}
prog() {
    name=$1; shift
    objs=
    for s in "$@"; do objs="$objs `cc1 $s`"; done
    env -u LPATH $HLD --sysroot=$SYSROOT -z -u main -o $OUT/$name \
        $SYSROOT/usr/lib/hpux64/unix98.o $objs $LIBGCC -lunwind -lc $LIBGCC
    echo "built $OUT/$name"
}

prog hld $LINKSRC
prog hld-readelf tools/hld_readelf.c $READER
prog hld-objcmp tools/hld_objcmp.c $READER
prog patch_harness tests/patch_harness.c $READER src/ia64_patch.c

for p in hld hld-readelf; do
    $HLD-readelf -d $OUT/$p | grep -E "NEEDED|RUNPATH" | sed "s|^|  $p: |"
done
