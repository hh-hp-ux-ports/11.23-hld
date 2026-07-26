#!/bin/sh
# Validate hld's ELF reader against the reference corpus in samples/gt1.
# POSIX sh — must also run on HP-UX itself (HP grep: no -A/-m/-o; plain BRE only).
# Run from the repo root: sh tests/check_readelf.sh

R=build/hld-readelf
FAIL=0
CHECKS=0
out=""

# need <label> <description> <grep-BRE>  — asserts $out matches the pattern
need() {
    CHECKS=`expr $CHECKS + 1`
    if printf '%s\n' "$out" | grep "$3" > /dev/null 2>&1; then
        :
    else
        echo "FAIL: $1: $2 (pattern: $3)"
        FAIL=1
    fi
}

# run <file> — full dump must succeed with silent stderr
run() {
    CHECKS=`expr $CHECKS + 1`
    if $R -a "$1" > /dev/null 2> .checktmp_err; then
        if [ -s .checktmp_err ]; then
            echo "FAIL: hld-readelf -a $1 wrote to stderr:"
            cat .checktmp_err
            FAIL=1
        fi
    else
        echo "FAIL: hld-readelf -a $1 exited nonzero"
        FAIL=1
    fi
    rm -f .checktmp_err
}

[ -x $R ] || { echo "build first: make"; exit 2; }

# 1. Every corpus file parses without error, full dump.
for f in samples/gt1/hello.o samples/gt1/hello_O0.o samples/gt1/hello_O2.o \
         samples/gt1/ret42.o samples/gt1/shlib_pic.o \
         samples/gt1/hello samples/gt1/hello_m samples/gt1/ret42 \
         samples/gt1/useshlib samples/gt1/libshlib.so; do
    run "$f"
done

# System binaries when present (not in git).
for f in samples/sys/sys_crt0.o samples/sys/sys_dld.so samples/sys/sys_libc.so.1; do
    [ -f "$f" ] && run "$f"
done

# 2. Pinned facts (from docs/format-notes.md).
out=`$R -a samples/gt1/hello`
need hello "OSABI HP-UX abi 1"      "HP-UX (1), ABI version 1"
need hello "flags TRAPNIL BE ABI64" "0x19 TRAPNIL BE ABI64"
need hello "entry = main 0xd80"     "Entry: *0x4000000000000d80"
need hello "interp colon list"      "uld.so:/usr/lib/hpux64/dld.so"
need hello "HP_LOAD_MAP tag"        "HP_LOAD_MAP"
need hello "PLT_RESERVE tag"        "IA_64_PLT_RESERVE"
need hello "RUNPATH resolves"       "RUNPATH.*/usr/ccs/lib/hpux64"
need hello "NEEDED libc resolves"   "NEEDED.*libc.so.1"
need hello "IPLT import printf"     "R_IA64_IPLTMSB.*printf"
need hello "gp symbol"              "__gp"
need hello "dlt section short-flag" "\.dlt .*WAp"
need hello "IA_64_UNWIND phdr"      "IA_64_UNWIND"
need hello "HP_FASTBIND phdr"       "HP_FASTBIND"
need hello "HP_LINKER_FOOTPRINT"    "HP_LINKER_FOOTPRINT"
need hello "PLTGOT is __gp"         "PLTGOT *0x6000000000000028"
need hello "printf UND hint value"  "40000000001e99e0.*printf"

out=`$R -a samples/gt1/hello.o`
need hello.o "REL type"             "REL (relocatable)"
need hello.o "flags BE ABI64 0x18"  "0x18 BE ABI64"
need hello.o "call reloc printf"    "R_IA64_PCREL21B.*printf"
need hello.o "gprel22 g_init"       "R_IA64_GPREL22.*g_init"
need hello.o "ltoff22x g_bss"       "R_IA64_LTOFF22X.*g_bss"
need hello.o "ldxmov present"       "R_IA64_LDXMOV"
need hello.o "unwind segrel"        "R_IA64_SEGREL64MSB"
need hello.o "unwind sht"           "IA_64_UNWIND"
need hello.o "sdata short flag"     "\.sdata .*WAp"

out=`$R -a samples/gt1/libshlib.so`
need libshlib "DYN type"            "DYN (shared object)"
need libshlib "dlt dyn reloc"       "R_IA64_DIR64MSB.*exported_var"
need libshlib "exported_fun sym"    "exported_fun"
need libshlib "soname absent"       "STRTAB"

out=`$R -a samples/gt1/ret42`
need ret42 "EXEC type"              "EXEC (executable)"
need ret42 "entry is main's addr"   "Entry: *0x4000000000000"

# 3. Section-name parity with GNU readelf (when a cross/native tool exists).
# Explicit CROSS_READELF env var, an optional untracked tests/local.conf (see
# tests/local.conf.example), a cross triple on PATH, or plain readelf if this
# happens to be running on ia64-hpux itself.
[ -f tests/local.conf ] && . tests/local.conf
CR=${CROSS_READELF:-`command -v ia64-hp-hpux11.23-readelf 2>/dev/null`}
if [ -z "$CR" ] && [ "`uname -s`" = "HP-UX" ]; then
    CR=`command -v readelf 2>/dev/null`
fi
if [ -n "$CR" ] && [ -x "$CR" ]; then
    for f in samples/gt1/hello samples/gt1/hello.o samples/gt1/libshlib.so; do
        ours=`$R -S $f`
        $CR -SW $f 2>/dev/null \
          | sed -n 's/^  \[ *[0-9][0-9]*\] \([^ ][^ ]*\) .*/\1/p' > .checktmp_names
        while read nm; do
            CHECKS=`expr $CHECKS + 1`
            case "$ours" in
            *"$nm"*) ;;
            *) echo "FAIL: $f: section $nm missing from our -S"; FAIL=1 ;;
            esac
        done < .checktmp_names
        rm -f .checktmp_names
    done
fi

if [ $FAIL -eq 0 ]; then
    echo "OK: all reader checks passed ($CHECKS checks)"
else
    echo "FAILURES present ($CHECKS checks)"
fi
exit $FAIL
