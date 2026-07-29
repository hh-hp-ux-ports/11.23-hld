#!/bin/sh
# Dynamic-linking tests.
#
# These link against the system C library and then RUN the result, so they
# only mean anything on the target platform: HP-UX on Itanium. Anywhere else
# they skip — cross-linking a dynamic image can be checked structurally, but
# whether the dynamic loader accepts it can only be answered by the loader.
#
# Run from the repo root: sh tests/check_dynamic.sh
# POSIX sh, and it must work under HP-UX's own utilities.

HLD=build/hld
RE=build/hld-readelf
W=build/dyntest
FAIL=0
CHECKS=0

[ -x $HLD ] || { echo "build first: make"; exit 2; }

sys=`uname -s 2>/dev/null`
mach=`uname -m 2>/dev/null`
if [ "$sys" != "HP-UX" ] || [ "$mach" != "ia64" ]; then
    echo "SKIP: dynamic tests require HP-UX/ia64 (running on $sys/$mach)"
    exit 0
fi

# An assembler for the startup fixture.
[ -f tests/local.conf ] && . tests/local.conf
if [ -z "$XAS" ]; then
    for cand in /opt/binutils/bin/as /opt/gnu/binutils2461/bin/as; do
        [ -x "$cand" ] && { XAS=$cand; break; }
    done
fi
if [ -z "$XAS" ] || [ ! -x "$XAS" ]; then
    echo "SKIP: no GNU assembler found (see tests/local.conf.example)"
    exit 0
fi

LIBDIR=/usr/lib/hpux64
if [ ! -f $LIBDIR/libc.so.1 ]; then
    echo "SKIP: $LIBDIR/libc.so.1 not present"
    exit 0
fi

mkdir -p $W

CHECKS=`expr $CHECKS + 1`
if $XAS -mlp64 -o $W/crt_min.o tests/asm/crt_min.s 2> $W/as.err; then :; else
    echo "FAIL: assembling tests/asm/crt_min.s:"; cat $W/as.err; exit 1
fi

# A gcc-compiled object that calls printf, from the reference corpus.
CHECKS=`expr $CHECKS + 1`
if $HLD -dynamic -e _start -o $W/hello \
        $W/crt_min.o samples/gt1/hello.o -L$LIBDIR -lc 2> $W/link.err; then :; else
    echo "FAIL: hld dynamic link:"; cat $W/link.err; exit 1
fi

# Structure the loader insists on (see docs/format-notes.md).
out=`$RE -l -d $W/hello`
need() {
    CHECKS=`expr $CHECKS + 1`
    if printf '%s\n' "$out" | grep "$2" > /dev/null 2>&1; then :; else
        echo "FAIL: $1 (pattern: $2)"
        FAIL=1
    fi
}
need "PT_INTERP names the loader"  "uld.so:/usr/lib/hpux64/dld.so"
need "DT_NEEDED records libc"      "NEEDED.*libc.so.1"
need "DT_HASH present"             "HASH"
need "DT_HP_LOAD_MAP present"      "HP_LOAD_MAP"
need "DT_IA_64_PLT_RESERVE"        "IA_64_PLT_RESERVE"
need "import relocations via RELA" "RELA "

# DT_JMPREL must NOT alias the DT_RELA array: under immediate binding the
# loader walks it twice and rejects the image.
CHECKS=`expr $CHECKS + 1`
if printf '%s\n' "$out" | grep "JMPREL" > /dev/null 2>&1; then
    echo "FAIL: DT_JMPREL is present; the loader rejects that under immediate binding"
    FAIL=1
fi

# INTERP must precede the loadable segments.
CHECKS=`expr $CHECKS + 1`
order=`printf '%s\n' "$out" | grep -n -E "^  (INTERP|LOAD)" | head -1`
case "$order" in
*INTERP*) ;;
*) echo "FAIL: a LOAD segment precedes PT_INTERP"; FAIL=1 ;;
esac

# The real test: it has to run, print, and exit cleanly.
CHECKS=`expr $CHECKS + 1`
got=`$W/hello 2> $W/run.err`
rc=$?
expected="hello-from-hld-groundtruth 46 0"
if [ $rc -ne 0 ]; then
    echo "FAIL: linked program exited $rc"
    [ -s $W/run.err ] && cat $W/run.err
    FAIL=1
elif [ "$got" != "$expected" ]; then
    echo "FAIL: wrong output"
    echo "  expected: $expected"
    echo "  got:      $got"
    FAIL=1
fi

# --- archives of the real toolchain --------------------------------------
# A program needing a libgcc helper, linked against the vendor archives. The
# helper itself calls into HP's millicode, so this crosses from a GNU archive
# into an HP one by transitive extraction.
GCCLIB=`ls -d /opt/gcc474/lib/gcc/ia64-hp-hpux11.23/*/hpux64 2>/dev/null | head -1`
if [ -n "$GCCLIB" ] && [ -f "$GCCLIB/libgcc.a" ] && [ -f /usr/lib/hpux64/milli.a ]; then
    cat > $W/divti.c <<'CEOF'
#include <stdio.h>
static __int128 big = (__int128)1000000007 * 1000000009;
int main(void) {
    __int128 q = big / 1000000007;
    printf("%llu\n", (unsigned long long)q);
    return (int)(q - 1000000009);
}
CEOF
    CHECKS=`expr $CHECKS + 1`
    if /opt/gcc474/bin/gcc -mlp64 -O0 -c $W/divti.c -o $W/divti.o 2> $W/cc.err; then
        CHECKS=`expr $CHECKS + 1`
        if $HLD -dynamic -e _start -o $W/divti $W/crt_min.o $W/divti.o \
                -L$GCCLIB -lgcc /usr/lib/hpux64/milli.a -L$LIBDIR -lc \
                2> $W/link.err; then
            CHECKS=`expr $CHECKS + 1`
            got=`$W/divti`; rc=$?
            if [ $rc -ne 0 ] || [ "$got" != "1000000009" ]; then
                echo "FAIL: libgcc/millicode program: exit $rc, output '$got'"
                FAIL=1
            fi
        else
            echo "FAIL: linking against libgcc.a and milli.a:"; cat $W/link.err
            FAIL=1
        fi
    else
        echo "FAIL: compiling the libgcc test:"; cat $W/cc.err
        FAIL=1
    fi
fi

# --- objects and archives from the vendor compiler -----------------------
# HP's compiler calls external functions through a descriptor (@pltoff) where
# gcc emits a direct branch, and its assembler leaves a non-zero placeholder
# in a field awaiting relocation. Both must work.
if [ -x /opt/aCC/bin/aCC ]; then
    cat > $W/hpmod.c <<'CEOF'
int hp_add(int a, int b) { return a + b; }
CEOF
    cat > $W/hpmain.c <<'CEOF'
#include <stdio.h>
extern int hp_add(int, int);
int main(void) { int r = hp_add(40, 2); printf("%d\n", r); return r - 42; }
CEOF
    CHECKS=`expr $CHECKS + 1`
    ( cd $W && /opt/aCC/bin/aCC -Ae +DD64 -c hpmod.c hpmain.c ) > $W/acc.log 2>&1
    if [ -f $W/hpmod.o ] && [ -f $W/hpmain.o ]; then
        ( cd $W && /usr/bin/ar rc libhp.a hpmod.o ) 2>/dev/null
        CHECKS=`expr $CHECKS + 1`
        if $HLD -dynamic -e _start -o $W/hpprog $W/crt_min.o $W/hpmain.o \
                -L$W -lhp -L$LIBDIR -lc 2> $W/link.err; then
            CHECKS=`expr $CHECKS + 1`
            got=`$W/hpprog`; rc=$?
            if [ $rc -ne 0 ] || [ "$got" != "42" ]; then
                echo "FAIL: vendor-compiler program: exit $rc, output '$got'"
                FAIL=1
            fi
        else
            echo "FAIL: linking vendor-compiler objects and archive:"
            cat $W/link.err
            FAIL=1
        fi
    else
        echo "FAIL: the vendor compiler did not produce objects:"; cat $W/acc.log
        FAIL=1
    fi
fi

# --- shared library output ------------------------------------------------
# A library is loaded wherever the loader puts it, so every linkage-table
# slot naming one of its own symbols is the loader's to fill, and hld must
# both emit the relocation and advertise the array it lives in. Getting the
# advertisement wrong is silent: the library loads, and reads whatever
# happens to be at the address it was linked for.
CC=/opt/gcc474/bin/gcc
[ -x $CC ] || CC=gcc
CHECKS=`expr $CHECKS + 1`
cat > $W/shlib.c <<'CEOF'
int lib_var = 7;
int *lib_addr(void) { return &lib_var; }
int lib_value(void) { return lib_var; }
CEOF
cat > $W/shmain.c <<'CEOF'
extern int *lib_addr(void);
extern int lib_value(void);
int main(void) { return (*lib_addr() == lib_value()) ? lib_value() : 1; }
CEOF
if $CC -mlp64 -fPIC -c $W/shlib.c -o $W/shlib.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b -o $W/libhldtest.so $W/shlib.o 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        out=`$RE -h -d $W/libhldtest.so`
        for pat in "DYN" "SONAME" "RELA "; do
            CHECKS=`expr $CHECKS + 1`
            if printf '%s\n' "$out" | grep "$pat" > /dev/null 2>&1; then :; else
                echo "FAIL: the shared library lacks $pat"
                FAIL=1
            fi
        done
        # The real test: a program links against it and reads through it.
        CHECKS=`expr $CHECKS + 1`
        if $CC -mlp64 -o $W/shprog $W/shmain.c -L$W -lhldtest 2> $W/link.err; then
            CHECKS=`expr $CHECKS + 1`
            SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/shprog
            rc=$?
            if [ $rc -ne 7 ]; then
                echo "FAIL: through the hld-built library, got $rc, expected 7"
                echo "      (the library's own data did not relocate with it)"
                FAIL=1
            fi
        else
            echo "FAIL: linking a program against the hld-built library:"
            cat $W/link.err
            FAIL=1
        fi
    else
        echo "FAIL: hld could not produce a shared library:"; cat $W/link.err
        FAIL=1
    fi
fi

# --- debug information ----------------------------------------------------
# Debug sections are not loaded, but dropping them is silent: the link
# succeeds and the result simply cannot be debugged. They have to be carried
# through and relocated, and their relocations sit at plain byte offsets
# rather than the bundle-and-slot offsets an instruction relocation uses.
CHECKS=`expr $CHECKS + 1`
cat > $W/dbg.c <<'CEOF'
#include <stdio.h>
static int helper(int x) { int y = x * 2; return y + 1; }
int main(void) { printf("%d\n", helper(20)); return 0; }
CEOF
if /opt/gcc474/bin/gcc -mlp64 -g -O0 -c $W/dbg.c -o $W/dbg.o 2> $W/cc.err ||
   gcc -mlp64 -g -O0 -c $W/dbg.c -o $W/dbg.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -dynamic -e _start -o $W/dbg $W/crt_min.o $W/dbg.o \
            -L$LIBDIR -lc 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        if $RE -S $W/dbg | grep '\.debug_info' > /dev/null 2>&1; then :; else
            echo "FAIL: .debug_info did not survive the link"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        got=`$W/dbg`
        if [ "$got" != "41" ]; then
            echo "FAIL: the -g program printed '$got', expected 41"
            FAIL=1
        fi
        # Where a debugger is installed, the real question is whether it can
        # put a breakpoint on a line of source.
        if [ -x /opt/langtools/bin/gdb ]; then
            CHECKS=`expr $CHECKS + 1`
            echo "break main" > $W/gdb.cmds
            echo "run" >> $W/gdb.cmds
            /opt/langtools/bin/gdb -nx --batch -x $W/gdb.cmds $W/dbg \
                > $W/gdb.out 2>&1
            if grep 'dbg\.c' $W/gdb.out > /dev/null 2>&1; then :; else
                echo "FAIL: the debugger could not place main in its source:"
                head -5 $W/gdb.out
                FAIL=1
            fi
        fi
    else
        echo "FAIL: linking a -g object:"; cat $W/link.err
        FAIL=1
    fi
fi

# --- C++: exceptions, iostreams, and another module's data ----------------
# A C++ program reads the C library's own data — the FILE table behind
# stdout, the ctype masks, errno — both through linkage-table slots and
# through data words that hold such an address. hld cannot resolve either at
# link time, so both need a dynamic relocation; without them the program
# dies on a null or read-only pointer inside the runtime's start-up, long
# before main. Exercised here end to end: construct, throw, unwind, catch.
CXX=
for cand in /opt/gcc474/bin/g++ g++; do
    if command -v "$cand" > /dev/null 2>&1; then CXX=$cand; break; fi
done
if [ -n "$CXX" ]; then
    mkdir -p $W/bdir
    rm -f $W/bdir/ld
    ln -s "`pwd`/$HLD" $W/bdir/ld
    cat > $W/ehtest.cpp <<'CEOF'
#include <iostream>
#include <stdexcept>
struct Trace {
    const char *n;
    Trace(const char *s) : n(s) {}
    ~Trace() { std::cout << "dtor " << n << std::endl; }
};
static void deep() { Trace t("deep"); throw std::runtime_error("boom"); }
int main() {
    std::cout << "start" << std::endl;
    try { deep(); }
    catch (const std::exception &e) { std::cout << "caught " << e.what() << std::endl; }
    return 0;
}
CEOF
    cat > $W/ehtest.expected <<'CEOF'
start
dtor deep
caught boom
CEOF
    CHECKS=`expr $CHECKS + 1`
    if $CXX -mlp64 -c $W/ehtest.cpp -o $W/ehtest.o 2> $W/cxx.err; then
        CHECKS=`expr $CHECKS + 1`
        if $CXX -mlp64 -B$W/bdir/ -o $W/ehtest $W/ehtest.o 2> $W/link.err; then
            CHECKS=`expr $CHECKS + 1`
            $W/ehtest > $W/ehtest.out 2> $W/run.err
            rc=$?
            if [ $rc -ne 0 ]; then
                echo "FAIL: C++ exception program exited $rc"
                [ -s $W/run.err ] && cat $W/run.err
                FAIL=1
            elif cmp -s $W/ehtest.out $W/ehtest.expected; then :; else
                echo "FAIL: C++ exception program printed the wrong thing:"
                cat $W/ehtest.out
                FAIL=1
            fi
        else
            echo "FAIL: linking a C++ program:"; cat $W/link.err
            FAIL=1
        fi
    else
        echo "FAIL: compiling the C++ test:"; cat $W/cxx.err
        FAIL=1
    fi
fi

if [ $FAIL -eq 0 ]; then
    echo "OK: dynamic link checks passed ($CHECKS checks) — printf through libc.so.1 ran"
else
    echo "FAILURES present ($CHECKS checks)"
fi
exit $FAIL
