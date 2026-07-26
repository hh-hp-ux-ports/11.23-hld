#!/bin/sh
# Static-link tests: hld must produce a structurally valid LP64 ET_EXEC.
#
# Structure is checked here on any host. Whether the result actually RUNS can
# only be settled on HP-UX/Itanium hardware:
#     sh tests/check_link.sh && <copy build/linktest/exit42 to an 11.23 box>
# The expected exit status is 42.
#
# POSIX sh — must also run on HP-UX (HP grep: plain BRE only).

HLD=build/hld
RE=build/hld-readelf
W=build/linktest
FAIL=0
CHECKS=0
out=""

need() {
    CHECKS=`expr $CHECKS + 1`
    if printf '%s\n' "$out" | grep "$3" > /dev/null 2>&1; then :; else
        echo "FAIL: $1: $2 (pattern: $3)"
        FAIL=1
    fi
}

[ -x $HLD ] || { echo "build first: make"; exit 2; }

[ -f tests/local.conf ] && . tests/local.conf
if [ -z "$XAS" ]; then
    XAS=`command -v ia64-hp-hpux11.23-as 2>/dev/null`
fi
if [ -z "$XAS" ] && [ -x /opt/binutils/bin/as ] && [ "`uname -s`" = "HP-UX" ]; then
    XAS=/opt/binutils/bin/as
fi
if [ -z "$XAS" ] || [ ! -x "$XAS" ]; then
    echo "SKIP: no ia64 gas found (link tests need one; see tests/local.conf.example)"
    exit 0
fi

mkdir -p $W
CHECKS=`expr $CHECKS + 1`
if $XAS -mlp64 -o $W/exit42.o tests/asm/exit42.s 2> $W/as.err; then :; else
    echo "FAIL: assembling tests/asm/exit42.s:"; cat $W/as.err; exit 1
fi

CHECKS=`expr $CHECKS + 1`
if $HLD -e _start -o $W/exit42 $W/exit42.o 2> $W/link.err; then :; else
    echo "FAIL: hld link:"; cat $W/link.err; exit 1
fi

out=`$RE -h -S -l $W/exit42`

need exec "ELF64 big-endian"        "ELF64, big-endian"
need exec "HP-UX OSABI, ABI ver 1"  "HP-UX (1), ABI version 1"
need exec "ET_EXEC"                 "EXEC (executable)"
need exec "IA-64 machine"           "IA-64 (50)"
need exec "e_flags BE|ABI64"        "0x18 BE ABI64"
need exec "entry in text segment"   "Entry: *0x4000000000000"
need exec "text LOAD at text base"  "LOAD *00000000 4000000000000000"
need exec "data LOAD at data base"  "LOAD *00010000 6000000000000000"
need exec "text segment is R E"     "4000000000000000.*R E"
need exec "data segment is RW"      "6000000000000000.*RW"
need exec ".text is AX"             "\.text .*AX"

# The kernel's loader requires (both proven by experiment on 11.23):
#  1. a writable LOAD segment must be present, even when it is empty;
#  2. every LOAD's p_offset must lie within the file — an offset past EOF is
#     rejected with "Exec format error" even when p_filesz is 0.
CHECKS=`expr $CHECKS + 1`
nload=`printf '%s\n' "$out" | grep -c "^  LOAD"`
if [ "$nload" -ne 2 ]; then
    echo "FAIL: expected 2 LOAD segments, found $nload"
    FAIL=1
fi

CHECKS=`expr $CHECKS + 1`
# data segment offset (col 2 of its LOAD line) must be inside the file
doff=`printf '%s\n' "$out" | grep "^  LOAD *00010000" | awk '{print $2}'`
fsize=`wc -c < $W/exit42`
if [ -n "$doff" ]; then
    dd=`echo $doff | awk '{printf "%d", strtonum("0x" $1)}' 2>/dev/null`
    [ -z "$dd" ] && dd=65536      # awk without strtonum: the known value
    if [ "$fsize" -lt "$dd" ]; then
        echo "FAIL: data p_offset $doff lies past end of file ($fsize bytes)"
        echo "      the kernel rejects this with Exec format error"
        FAIL=1
    fi
fi

if [ $FAIL -eq 0 ]; then
    echo "OK: static link checks passed ($CHECKS checks)"
    echo "    run $W/exit42 on HP-UX/Itanium; expected exit status 42"
else
    echo "FAILURES present ($CHECKS checks)"
fi
exit $FAIL
