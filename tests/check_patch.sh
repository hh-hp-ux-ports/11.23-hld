#!/bin/sh
# Golden-pair tests for the vendored IA-64 bundle patcher: gas assembles the
# same instruction with immediate 0/self-target (A) and with the real value
# (B); hld_ia64_install_value(A, value) must reproduce B byte-exactly.
# gas is the encoding oracle. POSIX sh.
# Run from the repo root: sh tests/check_patch.sh

H=build/patch_harness
W=build/patchtest
FAIL=0
CHECKS=0

# Locate an ia64 GNU assembler: explicit XAS env var, an optional untracked
# tests/local.conf (see tests/local.conf.example), a cross triple on PATH,
# or a native `as` when this script is itself running on ia64-hpux.
[ -f tests/local.conf ] && . tests/local.conf
if [ -z "$XAS" ]; then
    XAS=`command -v ia64-hp-hpux11.23-as 2>/dev/null`
fi
if [ -z "$XAS" ] && [ -x /opt/binutils/bin/as ] && [ "`uname -s`" = "HP-UX" ]; then
    XAS=/opt/binutils/bin/as
fi

[ -x $H ] || { echo "build first: make"; exit 2; }

CHECKS=`expr $CHECKS + 1`
if $H selftest; then :; else echo "FAIL: selftest"; FAIL=1; fi

if [ ! -x "$XAS" ]; then
    echo "SKIP: no ia64 gas found (pair tests need cross binutils); selftest only"
    exit $FAIL
fi

mkdir -p $W

emit() { # name body...
    printf '%s\n' "$2" > $W/$1.s
}

# --- fixtures -----------------------------------------------------------
emit movl0 '{ .mlx
  mov r35 = r1
  movl r36 = 0 ;;
}'
emit movl1 '{ .mlx
  mov r35 = r1
  movl r36 = 0x123456789abcdef0 ;;
}'
emit movl2 '{ .mlx
  mov r35 = r1
  movl r36 = 0xfedcba9876543210 ;;
}'
emit addl0 '{ .mii
  addl r36 = 0, r1
  nop.i 0
  nop.i 0 ;;
}'
emit addl1 '{ .mii
  addl r36 = 0x12345, r1
  nop.i 0
  nop.i 0 ;;
}'
emit addl2 '{ .mii
  addl r36 = -1234, r1
  nop.i 0
  nop.i 0 ;;
}'
emit addls10 '{ .mii
  nop.m 0
  addl r36 = 0, r1
  nop.i 0 ;;
}'
emit addls11 '{ .mii
  nop.m 0
  addl r36 = 0x54321, r1
  nop.i 0 ;;
}'
# HP's assembler leaves a non-zero placeholder in a field awaiting a
# relocation; patching must replace it, not merge into it.
emit dirty0 '{ .mii
  addl r36 = 591, r1
  nop.i 0
  nop.i 0 ;;
}'
emit dirty1 '{ .mii
  addl r36 = 0x12345, r1
  nop.i 0
  nop.i 0 ;;
}'
emit adds0 '{ .mii
  adds r14 = 0, r0
  nop.i 0
  nop.i 0 ;;
}'
emit adds1 '{ .mii
  adds r14 = 0x1abc, r0
  nop.i 0
  nop.i 0 ;;
}'
emit adds2 '{ .mii
  adds r14 = -42, r0
  nop.i 0
  nop.i 0 ;;
}'
emit call0 '.Ls:
{ .mib
  nop.m 0
  nop.i 0
  br.call.sptk.many b0 = .Ls ;;
}'
emit call1 '{ .mib
  nop.m 0
  nop.i 0
  br.call.sptk.many b0 = .Lt ;;
}
.skip 0x2000
.Lt:'
emit calln0 '.skip 0x100
.Ls:
{ .mib
  nop.m 0
  nop.i 0
  br.call.sptk.many b0 = .Ls ;;
}'
emit calln1 '.Lt:
.skip 0x100
{ .mib
  nop.m 0
  nop.i 0
  br.call.sptk.many b0 = .Lt ;;
}'
emit fchk0 '.Ls:
{ .mfb
  nop.m 0
  fchkf .Ls
  nop.b 0 ;;
}'
emit fchk1 '{ .mfb
  nop.m 0
  fchkf .Lt
  nop.b 0 ;;
}
.skip 0x2000
.Lt:'
emit brl0 '.Ls:
{ .mlx
  nop.m 0
  brl.call.sptk.many b0 = .Ls ;;
}'
emit brl1 '{ .mlx
  nop.m 0
  brl.call.sptk.many b0 = .Lt ;;
}
.skip 0x123440
.Lt:'

for f in movl0 movl1 movl2 addl0 addl1 addl2 addls10 addls11 \
         dirty0 dirty1 adds0 adds1 adds2 call0 call1 calln0 calln1 \
         fchk0 fchk1 \
         brl0 brl1; do
    if $XAS -mlp64 -o $W/$f.o $W/$f.s 2> $W/$f.err; then :; else
        echo "FAIL: gas on $f.s:"; cat $W/$f.err; FAIL=1
    fi
done
[ $FAIL -ne 0 ] && { echo "FAILURES present (fixture assembly)"; exit 1; }

pt() { # label args...
    lbl=$1; shift
    CHECKS=`expr $CHECKS + 1`
    if $H pair "$@"; then :; else echo "FAIL case: $lbl"; FAIL=1; fi
}

# --- cases --------------------------------------------------------------
# IMM64 movl (X-unit, slot 1)
pt imm64        $W/movl0.o 0 $W/movl1.o 0 1 0x23 0x123456789abcdef0
# GPREL64I on the SAME MLX shape = HP ld defect 1; slot 0 GP save must survive
pt gprel64i     $W/movl0.o 0 $W/movl2.o 0 1 0x2b 0xfedcba9876543210
# GPREL22 addl imm22, positive / negative / overflow
pt gprel22      $W/addl0.o 0 $W/addl1.o 0 0 0x2a 0x12345
pt gprel22neg   $W/addl0.o 0 $W/addl2.o 0 0 0x2a 0xfffffffffffffb2e
pt gprel22ovf   $W/addl0.o 0 $W/addl0.o 0 0 0x2a 0x200000 overflow
# patching over a non-zero placeholder must replace the field outright
pt imm22dirty   $W/dirty0.o 0 $W/dirty1.o 0 0 0x22 0x12345
# imm22 in SLOT 1 — the syllable spanning the t0/t1 dword boundary (bit 46)
pt gprel22slot1 $W/addls10.o 0 $W/addls11.o 0 1 0x2a 0x54321
# IMM14 adds, positive / negative
pt imm14        $W/adds0.o 0 $W/adds1.o 0 0 0x21 0x1abc
pt imm14neg     $W/adds0.o 0 $W/adds2.o 0 0 0x21 0xffffffffffffffd6
# PCREL21B br.call (B slot 2), forward and backward displacement
pt pcrel21b     $W/call0.o 0 $W/call1.o 0 2 0x49 0x2010
pt pcrel21bneg  $W/calln0.o 0x100 $W/calln1.o 0x100 2 0x49 0xffffffffffffff00
# PCREL60B brl (L+X, slot 1)
pt pcrel60b     $W/brl0.o 0 $W/brl1.o 0 1 0x48 0x123450
# PCREL21F fchkf (F slot 1) -- its own target encoding, distinct from the
# branch forms above. Added after a coverage sweep found it was the one
# instruction encoder the oracle never exercised.
pt pcrel21f     $W/fchk0.o 0 $W/fchk1.o 0 1 0x4b 0x2010

if [ $FAIL -eq 0 ]; then
    echo "OK: all patcher checks passed ($CHECKS checks, gas = $XAS)"
else
    echo "FAILURES present ($CHECKS checks)"
fi
exit $FAIL
