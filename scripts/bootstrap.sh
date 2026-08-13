#!/bin/sh
# Three-stage bootstrap: prove hld's output does not depend on which linker
# built hld.
#
#   sh scripts/bootstrap.sh [CC ...]
#
#   stage1   built with the system linker (HP's ld)
#   stage2   built with stage1's hld
#   stage3   built with stage2's hld
#
# stage2 and stage3 must be BYTE-IDENTICAL. They are compiled by the same
# compiler from the same sources, so the only thing that differs is which hld
# linked them; if the two disagree, an hld linked by hld is not the same
# program as an hld linked by HP's ld. stage1 is expected to differ from both
# and is not compared -- a different linker lays the image out differently,
# exactly as gcc's stage1 differs for having been built by a foreign compiler.
#
# WHAT THIS PROVES
#   - a fixed point: hld linked by hld behaves as hld linked by HP ld
#   - determinism: the output does not vary with run-to-run state, which
#     matters here because .dynsym and .symtab are emitted by walking symbol
#     hash tables
#
# WHAT THIS DOES NOT PROVE, and it is the larger half
#   hld is a C EXECUTABLE. It has no vtables, no exceptions, no thread-local
#   storage, no C++, and it is not a shared library. Every serious defect this
#   linker has had was in SHARED LIBRARY output -- undefined symbols, symbol
#   interposition, hidden symbols exported, a function address in static data,
#   another module's thread-local data, and the vtable descriptors fixed in
#   0.12.3. A green bootstrap would have caught NONE of them. Read it as a
#   determinism and self-consistency gate, never as evidence of correctness.
#
# The stage directory is normalised on purpose. gcc records the -B directory
# in the image's embedded library search path, so staging the three linkers at
# three different paths makes stage2 and stage3 differ by the one byte that
# spells the stage number, which reads exactly like a real defect. Every stage
# therefore links through the SAME path, $STAGE/ld, whose target is replaced
# between stages.

MAKE=${MAKE:-make}
W=build/bootstrap
STAGE=$W/cur                     # every stage links through this one path

# The compiler is fixed across all three stages; only the linker changes.
if [ $# -gt 0 ]; then
    CC="$*"
else
    CC=
    for cand in "/opt/gcc474/bin/gcc -mlp64" "gcc -mlp64" "gcc"; do
        set -- $cand
        if command -v "$1" > /dev/null 2>&1; then CC="$cand"; break; fi
    done
fi
if [ -z "$CC" ]; then
    echo "SKIP: bootstrap needs a C compiler that takes -B (none found)"
    exit 0
fi

case `uname -s` in
HP-UX) ;;
*)  echo "SKIP: bootstrap runs the linker it builds, so it needs HP-UX/ia64"
    exit 0 ;;
esac

rm -rf $W
mkdir -p $STAGE || exit 1

# Which linker actually ran? `-print-prog-name' reports a LOOKUP rather than
# an execution and has lied about this before; -Wl,-V is the only answer that
# comes from the linker itself.
linker_id() {
    _probe=$W/probe.c
    echo 'int main(void){return 0;}' > $_probe
    $CC $1 -o $W/probe $_probe -Wl,-V 2>&1 | head -3
}

stage_build() {                  # stage_build <outdir> <extra CC flags>
    mkdir -p $1 || return 1
    $MAKE BUILD=$1 CC="$CC $2" $1/hld > $1/build.log 2>&1
}

echo "bootstrap: CC = $CC"

# ---- stage1: the system linker ------------------------------------------
# If the compiler already defaults to hld -- gcc is often configured
# --with-ld for exactly that -- then stage1 is hld-linked too and the whole
# comparison degenerates into hld against itself. That has to be a hard error
# rather than a pass, because a vacuous bootstrap looks identical to a green
# one.
id1=`linker_id ""`
case "$id1" in
*hld*)
    echo "FAIL: this compiler already links with hld, so stage1 would not be"
    echo "      an independent starting point and the comparison would prove"
    echo "      nothing. Pass a compiler that defaults to the system linker:"
    echo "          sh scripts/bootstrap.sh /opt/gcc474/bin/gcc -mlp64"
    echo "      it reported: $id1"
    exit 1 ;;
esac
echo "bootstrap: stage1 linker is `echo \"$id1\" | head -1`"

if stage_build $W/s1 ""; then :; else
    echo "FAIL: stage1 did not build"; cat $W/s1/build.log; exit 1
fi

# ---- stage2: linked by stage1's hld --------------------------------------
ln -sf "`pwd`/$W/s1/hld" $STAGE/ld || exit 1
id2=`linker_id "-B\`pwd\`/$STAGE/"`
case "$id2" in
*hld*) ;;
*)  echo "FAIL: stage2 was not linked by hld -- -B did not take effect."
    echo "      it reported: $id2"
    exit 1 ;;
esac
if stage_build $W/s2 "-B`pwd`/$STAGE/"; then :; else
    echo "FAIL: stage2 did not build"; cat $W/s2/build.log; exit 1
fi

# ---- stage3: linked by stage2's hld --------------------------------------
# Same -B path as stage2, different target behind it. Copy rather than
# symlink to s2/hld: the path baked into the image must not vary.
cp $W/s2/hld $W/s2_hld || exit 1
ln -sf "`pwd`/$W/s2_hld" $STAGE/ld || exit 1
if stage_build $W/s3 "-B`pwd`/$STAGE/"; then :; else
    echo "FAIL: stage3 did not build"; cat $W/s3/build.log; exit 1
fi

# ---- the comparison ------------------------------------------------------
if cmp -s $W/s2/hld $W/s3/hld; then
    echo "OK: bootstrap reached a fixed point — stage2 and stage3 are"
    echo "    byte-identical (`wc -c < $W/s2/hld | tr -d ' '` bytes)."
    echo "    This is a determinism and self-consistency result only: hld is"
    echo "    an executable, so nothing here exercises shared-library output."
    exit 0
fi

echo "FAIL: stage2 and stage3 differ, so an hld linked by hld is not the"
echo "      same program as an hld linked by the system linker."
n=`cmp -l $W/s2/hld $W/s3/hld 2>/dev/null | wc -l | tr -d ' '`
echo "      differing bytes: $n"
cmp -l $W/s2/hld $W/s3/hld 2>/dev/null | head -5 | while read off a b; do
    echo "        offset $off (0x`printf '%x' $off`): $a -> $b"
done
echo "      If the count is tiny and the bytes spell a digit, check whether a"
echo "      PATH leaked into the image rather than suspecting the linker."
exit 1
