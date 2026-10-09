#!/bin/sh
# Relocatable-output tests (hld -r): the way an HP-UX kernel module is linked.
#
# The oracle is HP's own linker. tests/rel/obj holds hld's fixtures as the
# platform's tools built them, and r_*.o, what HP ld -r made of each set
# (tests/rel/mkref.sh, run on 11.23). hld -r must produce the same CONTRACT
# -- sections, contents, relocations, symbols, e_flags, compared by name by
# hld-objcmp -- not the same bytes. Two differences are expected and
# excluded everywhere: HP ld appends a linker-footprint record to .note, and
# hld stamps its version into .comment.
#
# Runs anywhere: the fixtures are committed objects, and nothing is loaded.
#
# POSIX sh -- must also run on HP-UX (HP grep: plain BRE only).

HLD=build/hld
CMP=build/hld-objcmp
RE=build/hld-readelf
F=tests/rel/obj
W=build/reltest
FAIL=0
CHECKS=0

[ -x $HLD ] && [ -x $CMP ] || { echo "build first: make"; exit 2; }
[ -f $F/r_pab.o ] || { echo "SKIP: no fixtures in $F"; exit 0; }
mkdir -p $W

fail() {
    echo "FAIL: $*"
    FAIL=1
}

# hld -r INPUTS..., then compare with HP's answer. $1 is a test name, $2 the
# reference, the rest hld's arguments.
same_as_hp() {
    name=$1; ref=$2; shift; shift
    CHECKS=`expr $CHECKS + 1`
    if $HLD -r -o $W/$name.o "$@" > $W/$name.err 2>&1; then :; else
        fail "$name: hld -r failed:"; cat $W/$name.err
        return
    fi
    if $CMP -x .note -x .comment $F/$ref $W/$name.o > $W/$name.cmp 2>&1; then :; else
        fail "$name: differs from HP ld -r ($F/$ref):"; cat $W/$name.cmp
    fi
}

# A link that must fail, with a message saying why.
must_fail() {
    name=$1; pattern=$2; shift; shift
    CHECKS=`expr $CHECKS + 1`
    if $HLD "$@" > $W/$name.err 2>&1; then
        fail "$name: hld succeeded where it must not"
    elif grep "$pattern" $W/$name.err > /dev/null 2>&1; then :; else
        fail "$name: no \`$pattern' in the diagnostic:"; cat $W/$name.err
    fi
}

# --- the comparison tool is not blind --------------------------------------
# Every verdict below rests on hld-objcmp reporting a difference when there
# is one. Two different HP outputs must differ, and an object must equal
# itself.
CHECKS=`expr $CHECKS + 1`
if $CMP $F/r_pab.o $F/r_pab.o > $W/self.cmp 2>&1; then :; else
    fail "hld-objcmp: an object differs from itself:"; cat $W/self.cmp
fi
CHECKS=`expr $CHECKS + 1`
if $CMP $F/r_pab.o $F/r_nfd.o > $W/blind.cmp 2>&1; then
    fail "hld-objcmp: sees no difference between two different objects"
elif grep "e_flags 0x18 vs 0x198" $W/blind.cmp > /dev/null; then :; else
    fail "hld-objcmp: missed the e_flags difference:"; cat $W/blind.cmp
fi

# --- same contract as HP ld -r ---------------------------------------------
# HP-as objects: locals of the same name in two inputs, a weak definition,
# COMMONs of two sizes, relocations against a local plus an addend.
same_as_hp pab r_pab.o $F/pa.o $F/pb.o
# kernel-model objects (e_flags 0x198) combine and keep their flags
same_as_hp nfd r_nfd.o $F/pa_nfd.o $F/pb_nfd.o
# -z marks the output TRAPNIL; nothing inherits it from an input
same_as_hp z r_z.o -z $F/pa.o
# gcc + GNU as: gp-relative short data, LTOFF22X/LDXMOV, FPTR64MSB, millicode
same_as_hp g r_g.o $F/g1.o $F/g2.o
# one section per function: sections stay apart by name, each with its own
# unwind table naming its own code section
same_as_hp gfs r_gfs.o $F/g1fs.o $F/g2fs.o
# gcc and HP cc objects in one output, HP debug sections carried
same_as_hp mix r_mix.o $F/g1.o $F/h2.o

# An archive is searched as in any link: pb.o is pulled in for `bar'.
rm -f $W/libpb.a
CHECKS=`expr $CHECKS + 1`
if (cd $W && cp ../../$F/pb.o pb.o && ar rc libpb.a pb.o) > $W/ar.err 2>&1; then
    same_as_hp ar r_ar.o $F/pa.o -L$W -lpb
else
    echo "note: no ar here, archive case not run"
fi

# --- a kernel module --------------------------------------------------------
# Plain -r: exactly what HP ld -r makes, .shtmod still PROGBITS.
same_as_hp tkm_plain r_tkm.o $F/tkm_body.o $F/tkm_meta.o $F/tkm_shtmod.o
# --kernel-module: the one difference is the one asked for -- .shtmod comes
# out as SHT_MOD (0x60000001), the type the module loader demands.
CHECKS=`expr $CHECKS + 1`
if $HLD -r --kernel-module -o $W/tkm.o $F/tkm_body.o $F/tkm_meta.o \
        $F/tkm_shtmod.o > $W/tkm.err 2>&1; then :; else
    fail "tkm: hld -r --kernel-module failed:"; cat $W/tkm.err
fi
$CMP -x .note -x .comment $F/r_tkm.o $W/tkm.o > $W/tkm.cmp 2>&1
CHECKS=`expr $CHECKS + 1`
if grep "^section \.shtmod: type 0x1 vs 0x60000001$" $W/tkm.cmp > /dev/null \
   && grep "^1 difference(s)$" $W/tkm.cmp > /dev/null; then :; else
    fail "tkm: expected exactly the .shtmod type to differ from HP ld -r:"
    cat $W/tkm.cmp
fi
CHECKS=`expr $CHECKS + 1`
if grep "warning" $W/tkm.err > /dev/null; then
    fail "tkm: a module built as HP builds one drew a warning:"; cat $W/tkm.err
fi
# HP's linker keeps an SHT_MOD input as it is (measured), so a module can be
# fed back in, even by plain -r; and doing so changes nothing but the stamp.
CHECKS=`expr $CHECKS + 1`
if $HLD -r -o $W/tkm2.o $W/tkm.o > $W/tkm2.err 2>&1; then
    if $CMP -x .comment $W/tkm.o $W/tkm2.o > $W/tkm2.cmp 2>&1; then :; else
        fail "tkm: relinking hld's own -r output changed it:"; cat $W/tkm2.cmp
    fi
else
    fail "tkm: hld -r cannot read its own output:"; cat $W/tkm2.err
fi

# --kernel-module refuses what the module loader cannot load.
must_fail kmod_gprel "GPREL22" -r --kernel-module -o $W/x.o \
    $F/tkm_sdata.o $F/tkm_meta.o $F/tkm_shtmod.o
must_fail kmod_milli "__milli" -r --kernel-module -o $W/x.o \
    $F/tkm_body.o $F/tkm_meta.o $F/tkm_shtmod.o $F/h2k.o
must_fail kmod_noshtmod "no \.shtmod" -r --kernel-module -o $W/x.o \
    $F/tkm_body.o $F/tkm_meta.o
must_fail kmod_nometa "KC_META" -r --kernel-module -o $W/x.o \
    $F/tkm_body.o $F/tkm_shtmod.o
must_fail kmod_needs_r "with -r" --kernel-module -o $W/x.o $F/tkm_body.o

# --- refusals HP ld -r makes too --------------------------------------------
# e_flags: a kernel object and an ordinary one, either order; then the
# floating-point model. HP: "Mismatched NOFUNCDESC_CONS_GP", "Mismatched
# REDUCEDFP".
must_fail nfd_mix "NOFUNCDESC_CONS_GP" -r -o $W/x.o $F/pa.o $F/pb_nfd.o
must_fail nfd_mix2 "NOFUNCDESC_CONS_GP" -r -o $W/x.o $F/pb_nfd.o $F/pa.o
must_fail rfp_mix "REDUCEDFP" -r -o $W/x.o $F/pa_rfp.o $F/pb.o
must_fail dup "duplicate definition of .pdat." -r -o $W/x.o $F/pa.o $F/pdup.o
must_fail r_shared "no meaning with -r" -r -b -o $W/x.o $F/pa.o
must_fail r_entry "no meaning with -r" -r -e foo -o $W/x.o $F/pa.o

# --- what -r keeps is enough to link from ----------------------------------
# Combine two objects with -r, then link a program from the result; it must
# come out exactly as the program linked from the two objects directly. That
# is the end-to-end proof that no relocation, addend or symbol was lost.
CHECKS=`expr $CHECKS + 1`
L=tests/obj
if [ -f $L/multi_a.o ] && [ -f $L/multi_b.o ] && [ -f $L/exit_stub.o ]; then
    if $HLD -r -o $W/multi_r.o $L/multi_a.o $L/multi_b.o > $W/mr.err 2>&1 \
       && $HLD -e _start -o $W/multi_via_r $W/multi_r.o $L/exit_stub.o \
            >> $W/mr.err 2>&1 \
       && $HLD -e _start -o $W/multi_direct $L/multi_a.o $L/multi_b.o \
            $L/exit_stub.o >> $W/mr.err 2>&1; then
        $CMP $W/multi_direct $W/multi_via_r > $W/mr.cmp 2>&1
        if grep "^section \.text\|^section \.data\|^relocs \|^header" \
                $W/mr.cmp > /dev/null; then
            fail "a program linked from -r output differs from one linked" \
                 "directly:"; cat $W/mr.cmp
        fi
        if [ "`uname -s`" = "HP-UX" ] && [ "`uname -m`" = "ia64" ]; then
            CHECKS=`expr $CHECKS + 1`
            $W/multi_via_r; rc=$?
            [ $rc -eq 42 ] || fail "multi_via_r exited $rc, not 42"
        fi
    else
        fail "linking through -r:"; cat $W/mr.err
    fi
fi

if [ $FAIL -eq 0 ]; then
    echo "OK: relocatable-output checks passed ($CHECKS checks)"
else
    exit 1
fi
