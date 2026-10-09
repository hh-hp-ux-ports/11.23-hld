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
# The round-trip objects rt_a.o/rt_b.o in tests/obj come from tests/asm,
# assembled like the other link fixtures there:
#   ia64-hp-hpux11.23-as -mlp64 -o tests/obj/rt_a.o tests/asm/rt_a.s
# and km_gnu.o (and fp_*.o) as gcc's kernel route does, with -mauto-pic.
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

# ... and a difference in contents and symbols, not only in the header:
# r_z.o is pa.o alone, so .text is shorter and `bar' is undefined there.
CHECKS=`expr $CHECKS + 1`
$CMP -x .note -x .comment $F/r_pab.o $F/r_z.o > $W/blind2.cmp 2>&1
if grep "^section \.text: size" $W/blind2.cmp > /dev/null \
   && grep "^symbol bar:" $W/blind2.cmp > /dev/null \
   && grep "^relocs \.data: " $W/blind2.cmp > /dev/null; then :; else
    fail "hld-objcmp: missed a contents, symbol or relocation difference:"
    cat $W/blind2.cmp
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
# Each refusal names the object, the section and offset, the function and
# the symbol -- on the guest the same module only gets "Relocation in
# module ... failed". g1.o is ordinary gcc code: gp-relative short data, a
# linkage-table load, a function descriptor.
CHECKS=`expr $CHECKS + 1`
$HLD -r --kernel-module -o $W/x.o $F/g1.o > $W/kmod_g1.err 2>&1
for want in \
    "g1.o: .text+0x1 (g1_entry+0x1): R_IA64_GPREL22 against \`.sbss'" \
    "g1.o: .text+0x50 (g1_entry+0x50): R_IA64_LTOFF22X against \`ext_var'" \
    "g1.o: .text+0x82 (g1_entry+0x82): R_IA64_GPREL22 against \`gdata'" \
    "g1.o: .sdata+0x0: R_IA64_FPTR64MSB against \`g1_entry'" \
    "5 relocation(s) the module loader does not process"; do
    grep "$want" $W/kmod_g1.err > /dev/null \
        || fail "kmod_g1: \`$want' not reported"
done
grep "LDXMOV" $W/kmod_g1.err > /dev/null \
    && fail "kmod_g1: LDXMOV, which the loader processes, was refused"
CHECKS=`expr $CHECKS + 1`
$HLD -r --kernel-module -o $W/x.o $F/tkm_body.o $F/tkm_meta.o \
    $F/tkm_shtmod.o $F/h2k.o > $W/kmod_h2k.err 2>&1
for want in \
    "h2k.o: .text+0x52 (ext_fn+0x52): call to millicode \`__milli_div32I'" \
    "h2k.o: .text+0x32 (g2_long+0x32): call to millicode \`__milli_remI'"; do
    grep "$want" $W/kmod_h2k.err > /dev/null \
        || fail "kmod_h2k: \`$want' not reported"
done
CHECKS=`expr $CHECKS + 1`
$HLD -r --kernel-module -o $W/x.o $F/tkm_sdata.o $F/tkm_meta.o \
    $F/tkm_shtmod.o > $W/kmod_sdata.err 2>&1
grep "tkm_sdata.o: .text+0x12 (tkm_load+0x12): R_IA64_GPREL22 against \`.sbss'" \
    $W/kmod_sdata.err > /dev/null \
    || { fail "kmod_sdata: the short-data access was not named:"; cat $W/kmod_sdata.err; }
must_fail kmod_noshtmod "no \.shtmod" -r --kernel-module -o $W/x.o \
    $F/tkm_body.o $F/tkm_meta.o
must_fail kmod_nometa "KC_META" -r --kernel-module -o $W/x.o \
    $F/tkm_body.o $F/tkm_shtmod.o
must_fail kmod_needs_r "with -r" --kernel-module -o $W/x.o $F/tkm_body.o

# --- a target release: the kernel's e_flags, whatever the inputs say ------
# A gcc/GNU-as object (0x98: no ABSOLUTE, no REDUCEDFP -- GNU as cannot
# spell them) with HP-as KC_META and .shtmod (0x198). Plain -r ors them to
# 0x198; --kernel-module=11.23 must give 0x198 and =11.31 0x1b8, with
# nothing but e_flags (and the SHT_MOD type) differing from plain -r.
G=tests/obj/km_gnu.o
CHECKS=`expr $CHECKS + 1`
if $HLD -r -o $W/km_plain.o $G $F/tkm_meta.o $F/tkm_shtmod.o \
        > $W/km.err 2>&1; then :; else
    fail "km: plain -r failed:"; cat $W/km.err
fi
for rel in 11.23 11.31; do
    case $rel in 11.23) want=0x198 ;; *) want=0x1b8 ;; esac
    CHECKS=`expr $CHECKS + 1`
    if $HLD -r --kernel-module=$rel -o $W/km_$rel.o $G $F/tkm_meta.o \
            $F/tkm_shtmod.o > $W/km_$rel.err 2>&1; then
        $CMP -x .comment $W/km_plain.o $W/km_$rel.o > $W/km_$rel.cmp 2>&1
        if [ "$rel" = 11.23 ]; then
            grep "^1 difference" $W/km_$rel.cmp > /dev/null \
                || fail "km $rel: more than .shtmod's type changed:" \
                        "`cat $W/km_$rel.cmp`"
        elif grep "^header: e_flags 0x198 vs $want$" $W/km_$rel.cmp \
                > /dev/null && grep "^2 difference" $W/km_$rel.cmp \
                > /dev/null; then :; else
            fail "km $rel: expected e_flags $want and .shtmod's type only:"
            cat $W/km_$rel.cmp
        fi
        $RE -h $W/km_$rel.o > $W/km_$rel.h
        grep "Flags: *$want " $W/km_$rel.h > /dev/null \
            || fail "km $rel: e_flags are not $want: `grep Flags $W/km_$rel.h`"
    else
        fail "km $rel: hld -r --kernel-module=$rel failed:"; cat $W/km_$rel.err
    fi
done
# 11.31's REDUCEDFP is a promise about the code: f0, f1, f6-f11 only. hld
# checks it instruction by instruction when it makes the promise. fp_wide
# breaks it nine times, in nine formats; each must be named, by object,
# section and offset, and none of its three conforming lines.
CHECKS=`expr $CHECKS + 1`
if $HLD -r --kernel-module=11.31 -o $W/x.o tests/obj/fp_wide.o \
        $F/tkm_meta.o $F/tkm_shtmod.o > $W/fp.err 2>&1; then
    fail "fp_wide: linked for 11.31 with f12..f40 in its code"
else
    n=`grep -c "^hld: tests/obj/fp_wide.o: .text+0x[0-9a-f]* (fp_wide+0x[0-9a-f]*): f[0-9]* is outside" $W/fp.err`
    [ "$n" -eq 9 ] && grep "9 instruction(s)" $W/fp.err > /dev/null \
        || { fail "fp_wide: expected 9 named instructions, got $n:"; cat $W/fp.err; }
    for want in "+0x1 .*f12" "+0x11 .*f40" "+0x21 .*f13" "+0x31 .*f14" \
                "+0x41 .*f2 " "+0x50 .*f32" "+0x51 .*f33" "+0x60 .*f15" \
                "+0x61 .*f16"; do
        grep "text$want" $W/fp.err > /dev/null \
            || fail "fp_wide: \`$want' not reported"
    done
fi
# The same code makes no such promise for 11.23 (0x198), so it links there.
CHECKS=`expr $CHECKS + 1`
$HLD -r --kernel-module=11.23 -o $W/x.o tests/obj/fp_wide.o $F/tkm_meta.o \
    $F/tkm_shtmod.o > $W/fp23.err 2>&1 \
    || { fail "fp_wide: refused for 11.23, which claims nothing about FP:"; cat $W/fp23.err; }

# An ordinary object is not kernel code, whatever release is named.
must_fail km_ordinary "lack NOFUNCDESC_CONS_GP" -r --kernel-module=11.23 \
    -o $W/x.o $F/pa.o $F/tkm_meta.o $F/tkm_shtmod.o
must_fail km_release "11.23 or 11.31" -r --kernel-module=11.11 -o $W/x.o $G

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
# is the end-to-end proof that no relocation, addend or symbol was lost:
# multi_* calls across objects (PCREL21B against globals); gp_* reaches
# short data gp-relatively and __gp by absolute address (GPREL22, IMM64)
# against a linker-defined symbol and a global; rt_* reaches its own data
# through local labels, i.e. GPREL22 and IMM64 against SECTION symbols,
# whose addends -r must shift.
L=tests/obj
for pair in multi gp rt; do
    CHECKS=`expr $CHECKS + 1`
    [ -f $L/${pair}_a.o ] && [ -f $L/${pair}_b.o ] && [ -f $L/exit_stub.o ] \
        || continue
    if $HLD -r -o $W/${pair}_r.o $L/${pair}_a.o $L/${pair}_b.o \
            > $W/$pair.rt.err 2>&1 \
       && $HLD -e _start -o $W/${pair}_via_r $W/${pair}_r.o $L/exit_stub.o \
            >> $W/$pair.rt.err 2>&1 \
       && $HLD -e _start -o $W/${pair}_direct $L/${pair}_a.o $L/${pair}_b.o \
            $L/exit_stub.o >> $W/$pair.rt.err 2>&1; then
        $CMP $W/${pair}_direct $W/${pair}_via_r > $W/$pair.rt.cmp 2>&1
        if grep "^section \|^relocs \|^header" $W/$pair.rt.cmp > /dev/null; then
            fail "$pair: a program linked from -r output differs from one" \
                 "linked directly:"; cat $W/$pair.rt.cmp
        fi
        if [ "`uname -s`" = "HP-UX" ] && [ "`uname -m`" = "ia64" ]; then
            CHECKS=`expr $CHECKS + 1`
            $W/${pair}_via_r; rc=$?
            [ $rc -eq 42 ] || fail "${pair}_via_r exited $rc, not 42"
        fi
    else
        fail "$pair: linking through -r:"; cat $W/$pair.rt.err
    fi
done

if [ $FAIL -eq 0 ]; then
    echo "OK: relocatable-output checks passed ($CHECKS checks)"
else
    exit 1
fi
