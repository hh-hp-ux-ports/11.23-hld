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
# A local.conf carried over from another machine names a tool that is not here;
# fall back to probing rather than reporting nothing was found.
[ -n "$XAS" ] && [ ! -x "$XAS" ] && XAS=
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

# --- thread-local storage, general-dynamic model --------------------------
# Code that cannot assume a variable is in its own module loads two table
# slots — the owning module and the offset within that module's block — and
# calls __tls_get_addr. In a program both are known at link time. The values
# are the platform linker's: module -1, offset from the same base a TPREL
# counts from. libstdc++ needs this, so a C++ link stops without it.
CHECKS=`expr $CHECKS + 1`
cat > $W/gd.c <<'CEOF'
#include <stdio.h>
static __thread int tls_a = 11;
static __thread int tls_b = 22;
int *get_a(void) { return &tls_a; }
int main(void) {
    int *a = get_a();
    printf("%d %d\n", *a, tls_b);
    return (*a == 11 && tls_b == 22) ? 0 : 1;
}
CEOF
CC=/opt/gcc474/bin/gcc
[ -x $CC ] || CC=gcc
if $CC -mlp64 -fPIC -O0 -c $W/gd.c -o $W/gd.o 2> $W/cc.err; then :; else
    echo "FAIL: could not compile the dynamic-TLS fixture:"; cat $W/cc.err
    FAIL=1
fi
if [ -f $W/gd.o ]; then
    # the compiler must actually have emitted the general-dynamic pair
    CHECKS=`expr $CHECKS + 1`
    if $RE -r $W/gd.o 2>/dev/null | grep 'DTPMOD' > /dev/null 2>&1; then :; else
        echo "NOTE: this compiler did not emit LTOFF_DTPMOD22; TLS check is weaker"
    fi
    mkdir -p $W/gdbin
    rm -f $W/gdbin/ld
    ln -s "`pwd`/$HLD" $W/gdbin/ld
    CHECKS=`expr $CHECKS + 1`
    if $CC -mlp64 -B$W/gdbin/ -o $W/gdprog $W/gd.o 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        got=`$W/gdprog`
        rc=$?
        if [ $rc -ne 0 ] || [ "$got" != "11 22" ]; then
            echo "FAIL: dynamic-TLS program: exit $rc, output '$got', expected '11 22'"
            FAIL=1
        fi
    else
        echo "FAIL: linking a program using general-dynamic TLS:"
        cat $W/link.err
        FAIL=1
    fi
fi

# --- the linker stamps what built the file --------------------------------
# Which linker produced a binary is the first question asked when one is
# suspected of building it wrong. The stamp is in the platform's `what`
# format, so `what` reports it for anything hld linked.
# Ask `what', not `strings': HP's strings does not report this stamp at all
# (measured — it finds nothing where GNU strings finds it), so the check used
# to pass or fail on which one came first on PATH. `what' is the tool the
# format exists for and is always present. Match the stamp TEXT rather than
# the string "hld", which also occurs in the path of anyone whose checkout is
# named after the linker.
CHECKS=`expr $CHECKS + 1`
if what $W/hello 2>/dev/null | grep 'LP64 linker for HP-UX' > /dev/null 2>&1
then :; else
    echo "FAIL: the linked program carries no linker identification"
    FAIL=1
fi
if [ -x /usr/bin/what ]; then
    CHECKS=`expr $CHECKS + 1`
    if /usr/bin/what $W/hello 2>/dev/null | grep 'hld ' > /dev/null 2>&1; then :; else
        echo "FAIL: what(1) does not report the linker that built it"
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
$CC -mlp64 -c $W/shmain.c -o $W/shmain.o 2> $W/cc.err
$CC -mlp64 -c $W/shmain.c -o $W/shmain.o 2> $W/cc.err
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

# --- a library's own data must be relocated at load time -------------------
# A string literal or a static has no global symbol, so its address in the
# linkage table cannot be relocated by naming it. The platform's linker names
# the SEGMENT it lives in and carries the rest in the addend; hld emitted
# nothing at all, so the slot kept its link-time value and the library handed
# back a pointer into whatever now occupies that address. Silent: it links
# clean and returns a plausible-looking pointer.
CHECKS=`expr $CHECKS + 1`
cat > $W/anch.c <<'CEOF'
static int counter = 41;
static struct { int a, b; } rec = { 3, 4 };
int         a_glob(void)   { return counter + 1; }
const char *a_str(void)    { return "anchored"; }
const void *a_rec(void)    { return &rec; }
int         a_recsum(void) { const int *p = (const int *)a_rec(); return p[0]+p[1]; }
CEOF
cat > $W/anchmain.c <<'CEOF'
#include <string.h>
extern int a_glob(void); extern const char *a_str(void); extern int a_recsum(void);
int main(void) {
    if (a_glob() != 42) return 1;
    if (strcmp(a_str(), "anchored") != 0) return 2;   /* .rodata via the DLT */
    if (a_recsum() != 7) return 3;                    /* .data  */
    return 7;
}
CEOF
if $CC -mlp64 -fPIC -c $W/anch.c -o $W/anch.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libanch.so -o $W/libanch.so $W/anch.o 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        $CC -mlp64 -o $W/anchprog $W/anchmain.c -L$W -lanch 2> $W/link.err
        SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/anchprog
        rc=$?
        if [ $rc -ne 7 ]; then
            echo "FAIL: library-local addresses not relocated (got $rc:"
            echo "      1=global 2=string literal 3=static struct)"
            FAIL=1
        fi
    else
        echo "FAIL: could not link the anchor fixture:"; cat $W/link.err; FAIL=1
    fi
fi

# --- one library is one dependency, whatever it was found as ---------------
# A library's identity is its SONAME, not the filename it was found under.
# Reached as libc.so from one -L and libc.so.1 from another it is still one
# dependency, and recording both puts the same name in DT_NEEDED twice.
if [ -d $LIBDIR ]; then
    CHECKS=`expr $CHECKS + 1`
    rm -rf $W/altlib; mkdir -p $W/altlib
    ln -s $LIBDIR/libc.so.1 $W/altlib/libc.so
    if $HLD -b +h libdup.so -o $W/dup.so $W/shlib.o \
            -L$W/altlib -lc -L$LIBDIR -lc 2> $W/link.err; then
        n=`$RE -d $W/dup.so 2>/dev/null | grep -c "libc.so.1"`
        if [ "$n" != "1" ]; then
            echo "FAIL: libc.so.1 recorded $n times in DT_NEEDED, expected 1"
            FAIL=1
        fi
    else
        echo "FAIL: linking one library under two names:"; cat $W/link.err; FAIL=1
    fi
fi

# --- a library's own function pointers need descriptors --------------------
# A pointer to one of the library's own functions is the address of a
# DESCRIPTOR {entry, gp}, and the loader writes the gp word -- so .opd has to
# be writable and in the data segment, the data word points at the descriptor,
# and the descriptor itself is relocated. Getting it wrong does not fault
# cleanly: control reaches something that is not the function.
CHECKS=`expr $CHECKS + 1`
cat > $W/fpt.c <<'CEOF'
static int f_a(void) { return 10; }
static int f_b(void) { return 20; }
static int f_c(void) { return 30; }
static int (* const tab[])(void) = { f_a, f_b, f_c };
int call_nth(int i) { return tab[i](); }
CEOF
cat > $W/fptmain.c <<'CEOF'
extern int call_nth(int);
int main(void) { return call_nth(2) == 30 ? 7 : 1; }
CEOF
if $CC -mlp64 -O2 -fPIC -c $W/fpt.c -o $W/fpt.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libfpt.so -o $W/libfpt.so $W/fpt.o 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        # .opd must be writable, or the loader cannot fill the gp word
        if $RE -S $W/libfpt.so 2>/dev/null | grep '\.opd' | grep 'WA' > /dev/null 2>&1
        then :; else
            echo "FAIL: .opd is not writable in a shared library"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        $CC -mlp64 -o $W/fptprog $W/fptmain.c -L$W -lfpt 2> $W/link.err
        SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/fptprog
        rc=$?
        if [ $rc -ne 7 ]; then
            echo "FAIL: calling through the library's own function-pointer"
            echo "      table gave $rc, expected 7"
            FAIL=1
        fi
    else
        echo "FAIL: could not link the descriptor fixture:"; cat $W/link.err; FAIL=1
    fi
fi

# --- an import is what the defining library says it is ---------------------
# Object files often reference a symbol without saying what it is: `extern int
# errno' arrives as NOTYPE. The library knows -- libc says errno is an OBJECT.
# Assuming FUNC builds a call descriptor for a variable, and the code then
# reads its data through a descriptor slot, which is a segfault.
CHECKS=`expr $CHECKS + 1`
cat > $W/ety.c <<'CEOF'
#include <errno.h>
int ety(void) { errno = 5; return errno; }
CEOF
if $CC -mlp64 -O2 -fPIC -c $W/ety.c -o $W/ety.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libety.so -o $W/libety.so $W/ety.o -L$LIBDIR -lc 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        ln=`$RE -s $W/libety.so 2>/dev/null \
            | awk '/\.dynsym/{f=1;next} /\.symtab/{f=0} f' | grep " errno$"`
        case "$ln" in
            *OBJECT*) : ;;
            *) echo "FAIL: errno imported as \"$ln\", expected OBJECT"; FAIL=1 ;;
        esac
        CHECKS=`expr $CHECKS + 1`
        n=`$RE -r $W/libety.so 2>/dev/null | grep -c "IPLT.*errno"`
        if [ "$n" != "0" ]; then
            echo "FAIL: a call descriptor was built for the variable errno"
            FAIL=1
        fi
    else
        echo "FAIL: could not link the import-type fixture:"; cat $W/link.err; FAIL=1
    fi
fi

# --- a local function's address formed in code -----------------------------
# Not read from a relocated data table: materialised by the code itself and
# stored somewhere the caller owns. The slot holding the descriptor's address
# still moves with the load, and without its relocation the code calls through
# a descriptor that is no longer there. This is zlib's deflateInit_ shape.
CHECKS=`expr $CHECKS + 1`
cat > $W/fna.c <<'CEOF'
typedef int (*alloc_f)(int,int);
typedef struct { alloc_f za; } strm_t;
static int fna_alloc(int n, int size) { return n * size + 1; }
int fna_init(strm_t *s) {
    if (s->za == (alloc_f)0) s->za = fna_alloc;
    return s->za(6, 7);
}
CEOF
cat > $W/fnamain.c <<'CEOF'
typedef int (*alloc_f)(int,int);
typedef struct { alloc_f za; } strm_t;
extern int fna_init(strm_t *);
int main(void) { strm_t s; s.za = 0; return fna_init(&s) == 43 ? 7 : 1; }
CEOF
if $CC -mlp64 -O2 -fPIC -c $W/fna.c -o $W/fna.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libfna.so -o $W/libfna.so $W/fna.o 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        $CC -mlp64 -o $W/fnaprog $W/fnamain.c -L$W -lfna 2> $W/link.err
        SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/fnaprog
        rc=$?
        if [ $rc -ne 7 ]; then
            echo "FAIL: a local function address formed in code gave $rc,"
            echo "      expected 7 (the descriptor-address slot is unrelocated)"
            FAIL=1
        fi
    else
        echo "FAIL: could not link the address-in-code fixture:"
        cat $W/link.err; FAIL=1
    fi
fi

# --- hidden visibility must not be exported --------------------------------
# libgcc's millicode and anything marked visibility("hidden") are not part of
# a library's interface. Exporting them offers them for interposition, which
# is how a program ends up calling a different library's division helper than
# the one it was built against.
CHECKS=`expr $CHECKS + 1`
cat > $W/vis.c <<'CEOF'
__attribute__((visibility("hidden"))) int vis_hidden(void) { return 1; }
int vis_public(void) { return vis_hidden() + 1; }
CEOF
if $CC -mlp64 -fPIC -c $W/vis.c -o $W/vis.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libvis.so -o $W/libvis.so $W/vis.o 2> $W/link.err; then
        dyn=`$RE -s $W/libvis.so 2>/dev/null \
             | awk '/\.dynsym/{f=1;next} /\.symtab/{f=0} f'`
        CHECKS=`expr $CHECKS + 1`
        if printf '%s\n' "$dyn" | grep vis_public > /dev/null 2>&1; then :; else
            echo "FAIL: the library does not export vis_public"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        if printf '%s\n' "$dyn" | grep vis_hidden > /dev/null 2>&1; then
            echo "FAIL: the library exports vis_hidden, which is hidden"
            FAIL=1
        fi
    else
        echo "FAIL: could not link the visibility fixture:"; cat $W/link.err; FAIL=1
    fi
fi

# --- a library must not export the linker's own layout symbols -------------
# `__gp', `_end', `_etext' and the rest describe one module's own layout. The
# platform's libraries export none of them (checked against libc.so.1 and
# libdl.so.1); a library that does lets another module bind to its addresses.
# An executable is the opposite case and must still export them, because the
# C library resolves `_end' and `main' there.
if [ -f $W/libhldtest.so ]; then
    CHECKS=`expr $CHECKS + 1`
    # Range must END at .symtab: the two tables are printed back to back with
    # no blank line between them, and .symtab legitimately still lists these.
    leaked=`$RE -s $W/libhldtest.so 2>/dev/null \
            | awk '/\.dynsym/{f=1;next} /\.symtab/{f=0} f' \
            | egrep -c '__gp$|_etext$|__text_start$|__data_start$|_end$'`
    if [ "$leaked" = "0" ] || [ -z "$leaked" ]; then :; else
        echo "FAIL: the shared library exports $leaked of the linker's own symbols"
        FAIL=1
    fi
fi

# --- run-time library search path -----------------------------------------
# The platform's linker records the -L list in the image, and programs depend
# on it: without it a binary that linked cleanly against a library outside the
# default directories cannot find it at startup and dies before main. Nothing
# in the link itself reveals this, so the check has to be a run with the
# environment deliberately empty.
if [ -f $W/libhldtest.so ]; then
    ADIR=`pwd`/$W
    CHECKS=`expr $CHECKS + 1`
    if $HLD -dynamic -e _start -o $W/rpprog $W/crt_min.o $W/shmain.o \
            -L$ADIR -lhldtest -L$LIBDIR -lc 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        if $RE -d $W/rpprog | grep RUNPATH > /dev/null 2>&1; then :; else
            echo "FAIL: no DT_RUNPATH, so the -L list was not recorded"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        SHLIB_PATH= LD_LIBRARY_PATH= export SHLIB_PATH LD_LIBRARY_PATH
        $W/rpprog
        rc=$?
        unset SHLIB_PATH LD_LIBRARY_PATH
        if [ $rc -ne 7 ]; then
            echo "FAIL: with no SHLIB_PATH set the program gave $rc, expected 7"
            echo "      (the loader could not find the library it was linked against)"
            FAIL=1
        fi
    else
        echo "FAIL: linking against a library outside the default path:"
        cat $W/link.err
        FAIL=1
    fi
    # +b names a directory explicitly, ahead of the -L defaults; and
    # +nodefaultrpath drops the defaults, as it does on the platform's linker.
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +b /run/first -o $W/rp2.so $W/shlib.o 2> $W/link.err; then
        got=`$RE -d $W/rp2.so | grep RUNPATH`
        case "$got" in
            *"/run/first"*) : ;;
            *) echo "FAIL: +b did not reach DT_RUNPATH: $got"; FAIL=1 ;;
        esac
    else
        echo "FAIL: +b was rejected:"; cat $W/link.err; FAIL=1
    fi
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +nodefaultrpath -L/should/not/appear -o $W/rp3.so $W/shlib.o \
            2> $W/link.err; then
        if $RE -d $W/rp3.so | grep RUNPATH > /dev/null 2>&1; then
            echo "FAIL: +nodefaultrpath still recorded the -L list"
            FAIL=1
        fi
    else
        echo "FAIL: +nodefaultrpath was rejected:"; cat $W/link.err; FAIL=1
    fi

# --- a library named on the command line is recorded ----------------------
# Recording only the libraries symbols are actually drawn from is --as-needed,
# which neither reference linker does by default. It breaks transitively: a
# library whose own RUNPATH cannot reach its dependencies is resolved through
# the executable's RUNPATH, and only while the executable names them itself.
# The program here deliberately calls nothing in the library it links against.
    CHECKS=`expr $CHECKS + 1`
    cat > $W/noref.c <<'CEOF'
int main(void) { return 0; }
CEOF
    if $CC -mlp64 -c $W/noref.c -o $W/noref.o 2> $W/cc.err; then
        CHECKS=`expr $CHECKS + 1`
        if $HLD -dynamic -e _start -o $W/noref $W/crt_min.o $W/noref.o \
                -L`pwd`/$W -lhldtest -L$LIBDIR -lc 2> $W/link.err; then
            CHECKS=`expr $CHECKS + 1`
            if $RE -d $W/noref | grep libhldtest > /dev/null 2>&1; then :; else
                echo "FAIL: a library named on the command line was not recorded"
                echo "      as DT_NEEDED because no symbol was drawn from it"
                FAIL=1
            fi
        else
            echo "FAIL: linking against an unreferenced library:"
            cat $W/link.err
            FAIL=1
        fi
    fi

# --- -l finds the platform's own shared-library suffix --------------------
# `.sl' is this platform's shared-library suffix and libraries still ship with
# only that name (GMP is one). Missing it does not fail the link: -lfoo falls
# through to libfoo.a and links that library statically instead, so the same
# command line silently builds a different program than the platform's linker
# does. The fixture puts both forms in one directory, archive included, and
# the shared one must win.
    CHECKS=`expr $CHECKS + 1`
    rm -rf $W/sldir; mkdir -p $W/sldir
    # Built, not copied: a library records the name it calls itself, and a
    # copy would still name the original.
    $HLD -b +h libsltest.sl -o $W/sldir/libsltest.sl $W/shlib.o 2> $W/link.err
    if $HLD -dynamic -e _start -o $W/slprog $W/crt_min.o $W/shmain.o \
            -L`pwd`/$W/sldir -lsltest -L$LIBDIR -lc 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        if $RE -d $W/slprog | grep NEEDED > /dev/null 2>&1; then :; else
            echo "FAIL: -lsltest did not resolve to libsltest.sl"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        SHLIB_PATH= LD_LIBRARY_PATH= export SHLIB_PATH LD_LIBRARY_PATH
        $W/slprog
        rc=$?
        unset SHLIB_PATH LD_LIBRARY_PATH
        if [ $rc -ne 7 ]; then
            echo "FAIL: the program linked against a .sl gave $rc, expected 7"
            FAIL=1
        fi
    else
        echo "FAIL: -l could not find a library named lib<name>.sl:"
        cat $W/link.err
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

# --- a static initialiser may hold an IMPORTED function's address -----------
# On this target a function address is a descriptor, and an import's belongs
# to the module that defines it -- so the loader writes it and the linker must
# not demand a local one. gnulib's allocator.c is exactly this
# (`{ malloc, realloc, free }`), so refusing it refused a large share of GNU
# packages. Calling through the pointers is the test: linking proves nothing
# if the descriptor the loader wrote is not the real one.
cat > $W/imp.c <<'CEOF'
#include <stdlib.h>
#include <string.h>
struct alloc { void *(*a)(size_t); void *(*r)(void *, size_t); void (*f)(void *); };
struct alloc const imp_allocator = { malloc, realloc, free };
int main(void) {
    char *p = imp_allocator.a(64);
    if (!p) return 1;
    strcpy(p, "ok");
    p = imp_allocator.r(p, 128);
    if (!p) return 2;
    imp_allocator.f(p);
    /* the canonical descriptor: what the loader wrote must be what we see */
    if ((void *)imp_allocator.a != (void *)malloc) return 3;
    if ((void *)imp_allocator.r != (void *)realloc) return 4;
    if ((void *)imp_allocator.f != (void *)free) return 5;
    return 0;
}
CEOF
if $CC -mlp64 -O2 -c $W/imp.c -o $W/imp.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -dynamic -e _start -o $W/impprog $W/crt_min.o $W/imp.o \
            -L$LIBDIR -lc 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        $W/impprog
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "FAIL: imported function pointers in static data: exit $rc"
            echo "      (1/2 allocation failed, 3/4/5 pointer is not the"
            echo "       canonical descriptor for malloc/realloc/free)"
            FAIL=1
        fi
    else
        echo "FAIL: a static initialiser holding imported addresses:"
        cat $W/link.err
        FAIL=1
    fi
fi

# --- an export list is a library's alone ------------------------------------
# An executable's exported symbols are not an interface anyone chooses: the C
# library binds `_end' there. Restricting them produces an image the loader
# refuses, and the link reports success, so only running it catches this.
CHECKS=`expr $CHECKS + 1`
cat > $W/xe.c <<'CEOF'
#include <stdio.h>
int main(void) { printf("xe-ran\n"); return 0; }
CEOF
if $CC -mlp64 -c $W/xe.c -o $W/xe.o 2> $W/cc.err; then
    if $HLD -dynamic -e main +e main -o $W/xeprog $W/xe.o -L$LIBDIR -lc \
            2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        got=`$W/xeprog 2>&1`
        if [ "$got" != "xe-ran" ]; then
            echo "FAIL: +e on an executable broke it: '$got'"
            echo "      (an executable's exports are the C library's business)"
            FAIL=1
        fi
    else
        echo "FAIL: +e on an executable did not link:"; cat $W/link.err; FAIL=1
    fi
fi

# --- an option that takes an argument must not be prefix-matched ------------
# HP's +vtype takes one. Swallowing it as a diagnostic-only `+v' switch leaves
# the argument to be read as an input file -- silently linking one if a file
# of that name exists.
CHECKS=`expr $CHECKS + 1`
if $HLD +vtype shared -o $W/vt.out $W/xe.o 2> $W/link.err; then
    echo "FAIL: +vtype was accepted; its argument becomes an input file"
    FAIL=1
elif grep 'shared' $W/link.err > /dev/null 2>&1; then
    echo "FAIL: +vtype left its argument to be read as a file:"
    cat $W/link.err
    FAIL=1
fi
CHECKS=`expr $CHECKS + 1`
if $HLD +vshlibunsats -dynamic -e main -o $W/vt.out $W/xe.o -L$LIBDIR -lc \
        2> $W/link.err; then :; else
    echo "FAIL: the argument-less +v switches must still be accepted:"
    cat $W/link.err
    FAIL=1
fi

# --- a far call to an exported symbol reaches its stub ----------------------
# The sizing pass and the relocation pass have to measure the same address.
# An exported symbol the library also calls is reached through a stub placed
# after the whole text, so a call NEAR the target can still be far from the
# stub: measuring the target instead decides no long-branch stub is needed,
# and the link then fails at relocation. Needs 20 MB of text to show up.
CHECKS=`expr $CHECKS + 1`
cat > $W/farlib.c <<'CEOF'
int far_exported(void) { return 42; }
int far_caller(void) { return far_exported(); }
CEOF
# The padding is its own object, listed after the code, so the order is
# certain: [callee][caller][20 MB]. Inline asm in the C file is emitted
# wherever the compiler likes -- which put the filler FIRST and left caller,
# callee and stub adjacent, testing nothing.
printf '\t.text\n\t.skip 0x1400000\n' > $W/farpad.s
$XAS -mlp64 -o $W/farpad.o $W/farpad.s 2> $W/as.err
cat > $W/farmain.c <<'CEOF'
extern int far_caller(void);
int main(void) { return far_caller() - 42; }
CEOF
if $CC -mlp64 -O2 -fPIC -c $W/farlib.c -o $W/farlib.o 2> $W/cc.err; then
    if $HLD -b +h libfar.so -o $W/libfar.so $W/farlib.o $W/farpad.o \
            2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        $CC -mlp64 -o $W/farprog $W/farmain.c -L$W -lfar 2> $W/link.err
        SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/farprog
        if [ $? -ne 0 ]; then
            echo "FAIL: a far call to an exported symbol did not run"
            FAIL=1
        fi
    else
        echo "FAIL: linking a library whose exported call needs a stub:"
        cat $W/link.err
        FAIL=1
    fi
fi

# --- a FAR call to an exported symbol must interpose too --------------------
# The intersection of two features that were tested only separately: a call
# beyond a branch's reach goes through a long-branch stub, and a call to an
# exported symbol goes through the descriptor stub. Three passes pick that
# target -- sizing, relocation, and stub WRITING -- and if the writing pass
# disagrees the far call lands on the local definition while a near call to
# the same symbol interposes. One function, two identities, and only in
# libraries past 16 MB, so nothing smaller reveals it.
cat > $W/fari.c <<'CEOF'
int fari_callee(void);
int fari_caller(void) { return fari_callee(); }
int fari_callee(void) { return 1; }
CEOF
cat > $W/farimain.c <<'CEOF'
extern int fari_caller(void);
int fari_callee(void) { return 2; }      /* the program replaces it */
int main(void) { return fari_caller() == 2 ? 0 : 1; }
CEOF
printf '\t.text\n\t.skip 0x1400000\n' > $W/faripad.s
if $XAS -mlp64 -o $W/faripad.o $W/faripad.s 2> $W/as.err \
   && $CC -mlp64 -O2 -fPIC -c $W/fari.c -o $W/fari.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libfari.so -o $W/libfari.so $W/fari.o $W/faripad.o \
            2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        $CC -mlp64 -o $W/fariprog $W/farimain.c -L$W -lfari 2> $W/link.err
        SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/fariprog
        if [ $? -ne 0 ]; then
            echo "FAIL: a far call to an exported symbol did not interpose"
            echo "      (the long-branch stub went to the local definition)"
            FAIL=1
        fi
    else
        echo "FAIL: linking the far-interposition fixture:"
        cat $W/link.err
        FAIL=1
    fi
fi

# --- a library's own exported calls stay interposable -----------------------
# A program may replace a symbol its library uses internally -- C++ requires
# exactly that for operator new. Binding those calls at link time instead
# produces a library that works, links cleanly, and quietly ignores the
# replacement. The pointer test matters as much: if the call interposes but
# the address does not, one function has two addresses in one process.
cat > $W/int.c <<'CEOF'
int int_impl(void) { return 1; }
typedef int (*int_fp)(void);
int int_direct(void) { return int_impl(); }
int_fp int_addr(void) { return int_impl; }
/*
 * Exported and only ever address-taken -- never called from inside the
 * library. Marking a symbol interposable on calls alone leaves this one
 * bound at link time, and the pointer the library hands out is its own
 * rather than the one the rest of the process uses.
 */
int int_ptr_only(void) { return 1; }
static int_fp int_table[1] = { int_ptr_only };
int_fp int_from_table(void) { return int_table[0]; }
CEOF
cat > $W/intmain.c <<'CEOF'
#include <stdio.h>
typedef int (*int_fp)(void);
extern int int_direct(void);
extern int_fp int_addr(void);
extern int_fp int_from_table(void);
int int_impl(void) { return 2; }          /* the program replaces it */
int int_ptr_only(void) { return 2; }      /* and this one too */
int main(void) {
    int_fp p = int_addr();
    int_fp q = int_from_table();
    printf("%d %d %d %d\n", int_direct(), p(),
           (void *)p == (void *)int_impl, q());
    return 0;
}
CEOF
if $CC -mlp64 -O2 -fPIC -c $W/int.c -o $W/int.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libint.so -o $W/libint.so $W/int.o 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        $CC -mlp64 -o $W/intprog $W/intmain.c -L$W -lint 2> $W/link.err
        got=`SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/intprog`
        if [ "$got" != "2 2 1 2" ]; then
            echo "FAIL: interposition: got '$got', expected '2 2 1 2'"
            echo "      (direct call, via pointer, addresses equal,"
            echo "       and a symbol only ever address-taken)"
            FAIL=1
        fi
    else
        echo "FAIL: linking the interposition fixture:"; cat $W/link.err; FAIL=1
    fi
    # -B symbolic is the opt-out, and has to actually opt out
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libint.so -B symbolic -o $W/libint.so $W/int.o \
            2> $W/link.err; then
        $CC -mlp64 -o $W/intprog2 $W/intmain.c -L$W -lint 2> $W/link.err
        got=`SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/intprog2`
        if [ "$got" != "1 1 0 1" ]; then
            echo "FAIL: -B symbolic: got '$got', expected '1 1 0 1'"
            FAIL=1
        fi
    else
        echo "FAIL: -B symbolic was not accepted:"; cat $W/link.err; FAIL=1
    fi
fi

# --- +e decides what a library offers ---------------------------------------
# Naming any symbol restricts the export list to those named, which is the
# platform linker's rule. Calls to a symbol left out still resolve inside the
# module -- only the offer to other modules is withdrawn, so the library has
# to keep working, not merely link.
cat > $W/exp.c <<'CEOF'
int exp_internal(int x) { return x * 2; }        /* global, not exported */
int exp_public(int x) { return exp_internal(x) + 40; }
CEOF
cat > $W/expmain.c <<'CEOF'
extern int exp_public(int);
int main(void) { return exp_public(1) - 42; }
CEOF
if $CC -mlp64 -O2 -fPIC -c $W/exp.c -o $W/exp.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libexp.so +e exp_public -o $W/libexp.so $W/exp.o \
            2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        if $RE -s $W/libexp.so 2>/dev/null | grep -w exp_public \
           > /dev/null 2>&1; then :; else
            echo "FAIL: +e did not export the symbol it named"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        # the whole point: everything NOT named must be withheld
        # only .dynsym is restricted -- .symtab still defines it. A sed
        # range would run to EOF here and read the wrong table.
        if $RE -s $W/libexp.so 2>/dev/null \
           | awk '/Symbol table .\.dynsym/{d=1;next} /Symbol table/{d=0} d' \
           | grep -w exp_internal > /dev/null 2>&1; then
            echo "FAIL: +e named one symbol but exp_internal is still exported"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        # and the library must still work: the internal call has to resolve
        $CC -mlp64 -o $W/expprog $W/expmain.c -L$W -lexp 2> $W/link.err
        SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/expprog
        if [ $? -ne 0 ]; then
            echo "FAIL: a library with a restricted export list stopped working"
            FAIL=1
        fi
    else
        echo "FAIL: linking with +e:"; cat $W/link.err; FAIL=1
    fi
    # +hideallsymbols restricts without naming anything
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libhide.so +hideallsymbols -o $W/libhide.so $W/exp.o \
            2> $W/link.err; then
        n=`$RE -s $W/libhide.so 2>/dev/null \
           | awk '/Symbol table .\.dynsym/{d=1;next} /Symbol table/{d=0} d' \
           | grep -c "exp_public\|exp_internal"`
        if [ "$n" != "0" ]; then
            echo "FAIL: +hideallsymbols still exported $n symbol(s)"
            FAIL=1
        fi
    else
        echo "FAIL: linking with +hideallsymbols:"; cat $W/link.err; FAIL=1
    fi
    # without either option nothing changes
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h liball.so -o $W/liball.so $W/exp.o 2> $W/link.err; then
        if $RE -s $W/liball.so 2>/dev/null | grep -w exp_internal \
           > /dev/null 2>&1; then :; else
            echo "FAIL: without +e, exp_internal should still be exported"
            FAIL=1
        fi
    fi
    # a name nothing defines is a typo worth hearing about
    CHECKS=`expr $CHECKS + 1`
    $HLD -b +h libw.so +e exp_public +e no_such_symbol -o $W/libw.so \
         $W/exp.o 2> $W/link.err
    if grep 'no_such_symbol' $W/link.err > /dev/null 2>&1; then :; else
        echo "FAIL: +e naming an undefined symbol said nothing"
        FAIL=1
    fi
fi

# --- static functions and variables keep their names ------------------------
# A `static' is never interned globally, so unless the output symbol table is
# built from each object's own symbols it has no name at all: `nm', a profiler
# and a crash dump can say nothing about it. Debug info is separate and was
# never affected -- this is about a binary built without -g.
cat > $W/loc.c <<'CEOF'
static int hld_local_tab[4] = {1, 2, 3, 4};
static int hld_local_fn(int x) { return x + hld_local_tab[0]; }
int main(void) { return hld_local_fn(1) - 2; }
CEOF
if $CC -mlp64 -O0 -c $W/loc.c -o $W/loc.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    if $HLD -dynamic -e _start -o $W/locprog $W/crt_min.o $W/loc.o \
            -L$LIBDIR -lc 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        for s in hld_local_fn hld_local_tab; do
            if $RE -s $W/locprog 2>/dev/null | grep -w $s | grep LOCAL \
               > /dev/null 2>&1; then :; else
                echo "FAIL: static \`$s' has no LOCAL entry in .symtab"
                FAIL=1
            fi
        done
        # ELF requires every local before any global, and sh_info says where
        # the globals start -- a table that violates it misleads every reader.
        CHECKS=`expr $CHECKS + 1`
        $RE -s $W/locprog 2>/dev/null | awk '
            /Symbol table .\.symtab/ { insym = 1; n = 0; next }
            /Symbol table/           { insym = 0 }
            insym && /LOCAL/         { n++; lastlocal = n }
            insym && /GLOBAL|WEAK/   { n++; if (!firstglobal) firstglobal = n }
            END { if (firstglobal && lastlocal > firstglobal) exit 1; exit 0 }
        ' || {
            echo "FAIL: a GLOBAL precedes a LOCAL in .symtab"
            FAIL=1
        }
        CHECKS=`expr $CHECKS + 1`
        $W/locprog
        if [ $? -ne 0 ]; then
            echo "FAIL: program with statics did not run correctly"
            FAIL=1
        fi
    else
        echo "FAIL: linking the statics fixture:"; cat $W/link.err; FAIL=1
    fi
fi

# --- a library may leave symbols for the loader to bind ---------------------
# Leaving a symbol undefined is what a shared library is for: gcc's LIB_SPEC
# is %{!shared:...}, so a plain `gcc -shared' passes no -lc at all. The types
# matter as much as the link succeeding -- a compiler marks a function it
# calls FUNC and leaves an undefined variable NOTYPE, and giving the variable
# a descriptor is how a data reference ends up going through one.
cat > $W/undef.c <<'CEOF'
extern int printf(const char *, ...);
extern int shared_counter;              /* data: the program defines it */
int undef_val(void)
{
    shared_counter += 5;
    printf("%d\n", shared_counter);
    return shared_counter;
}
CEOF
cat > $W/undefmain.c <<'CEOF'
int shared_counter = 2;
extern int undef_val(void);
int main(void) { return undef_val(); }
CEOF
if $CC -mlp64 -O2 -fPIC -c $W/undef.c -o $W/undef.o 2> $W/cc.err; then
    CHECKS=`expr $CHECKS + 1`
    # no -lc, and nothing else defines either symbol
    if $HLD -b +h libundef.so -o $W/libundef.so $W/undef.o 2> $W/link.err; then
        CHECKS=`expr $CHECKS + 1`
        # the called function keeps FUNC; the variable must stay NOTYPE
        t=`$RE -s $W/libundef.so 2>/dev/null \
           | grep -w printf | head -1 | awk '{print $4}'`
        if [ "$t" != "FUNC" ]; then
            echo "FAIL: imported printf typed '$t', expected FUNC"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        t=`$RE -s $W/libundef.so 2>/dev/null \
           | grep -w shared_counter | head -1 | awk '{print $4}'`
        if [ "$t" = "FUNC" ]; then
            echo "FAIL: imported variable shared_counter typed FUNC;"
            echo "      a data reference would go through a descriptor"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        # exactly one descriptor: for the function, not the variable
        n=`$RE -r $W/libundef.so 2>/dev/null | grep -c IPLTMSB`
        if [ "$n" != "1" ]; then
            echo "FAIL: $n descriptors for one imported function, expected 1"
            FAIL=1
        fi
        CHECKS=`expr $CHECKS + 1`
        # and it must actually bind and run: 2 + 5
        $CC -mlp64 -o $W/undefprog $W/undefmain.c -L$W -lundef 2> $W/link.err
        SHLIB_PATH=$W LD_LIBRARY_PATH=$W $W/undefprog
        rc=$?
        if [ $rc -ne 7 ]; then
            echo "FAIL: library with undefined symbols exited $rc, expected 7"
            FAIL=1
        fi
    else
        echo "FAIL: a shared library may leave symbols undefined:"
        cat $W/link.err
        FAIL=1
    fi
    # +noallowunsats asks for the opposite bargain: report now rather than
    # let the loader discover it. That is what a C++ module wants -- one that
    # throws needs __cxa_allocate_exception, and where no shared libstdc++
    # exists it would otherwise link cleanly and fail at dlopen.
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libundef.so +noallowunsats -o $W/libno.so $W/undef.o \
            2> $W/link.err; then
        echo "FAIL: +noallowunsats accepted a library with unsatisfied symbols"
        FAIL=1
    else
        # every unsatisfied symbol, not merely the first: answering them one
        # link at a time is one build per symbol. Naming them also keeps a
        # linker that simply rejects the option from passing this.
        CHECKS=`expr $CHECKS + 1`
        for s in printf shared_counter; do
            if grep "$s" $W/link.err > /dev/null 2>&1; then :; else
                echo "FAIL: +noallowunsats did not report '$s':"
                cat $W/link.err
                FAIL=1
            fi
        done
    fi
    # the GNU spelling does the same
    CHECKS=`expr $CHECKS + 1`
    if $HLD -b +h libundef.so --no-undefined -o $W/libno.so $W/undef.o \
            2> $W/link.err; then
        echo "FAIL: --no-undefined accepted unsatisfied symbols"
        FAIL=1
    fi

    # ...but a program may not: nothing is loaded after it to supply one.
    CHECKS=`expr $CHECKS + 1`
    cat > $W/nodef.c <<'CEOF'
extern int nowhere_at_all(void);
int main(void) { return nowhere_at_all(); }
CEOF
    if $CC -mlp64 -c $W/nodef.c -o $W/nodef.o 2> $W/cc.err; then
        if $HLD -o $W/nodef $W/nodef.o -L$LIBDIR -lc 2> $W/link.err; then
            echo "FAIL: a program with an undefined symbol linked anyway"
            FAIL=1
        elif grep 'undefined symbol' $W/link.err > /dev/null 2>&1; then :; else
            echo "FAIL: undefined symbol in a program gave the wrong error:"
            cat $W/link.err
            FAIL=1
        fi
    fi
fi

if [ $FAIL -eq 0 ]; then
    echo "OK: dynamic link checks passed ($CHECKS checks) — printf through libc.so.1 ran"
else
    echo "FAILURES present ($CHECKS checks)"
fi
exit $FAIL
