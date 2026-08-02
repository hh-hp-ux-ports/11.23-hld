#!/bin/sh
# Archive tests: an archive is searched at its position on the command line,
# and only members that satisfy something undefined are pulled in.
#
# Needs an ia64 assembler and `ar`. On HP-UX/Itanium the linked programs are
# also run; elsewhere their structure is checked. Run from the repo root.
# POSIX sh, HP-safe grep.

HLD=build/hld
RE=build/hld-readelf
W=build/artest
FAIL=0
CHECKS=0

[ -x $HLD ] || { echo "build first: make"; exit 2; }

[ -f tests/local.conf ] && . tests/local.conf
# A local.conf carried over from another machine names a tool that is not here;
# fall back to probing rather than reporting nothing was found.
[ -n "$XAS" ] && [ ! -x "$XAS" ] && XAS=
if [ -z "$XAS" ]; then
    XAS=`command -v ia64-hp-hpux11.23-as 2>/dev/null`
fi
if [ -z "$XAR" ]; then
    XAR=`command -v ia64-hp-hpux11.23-ar 2>/dev/null`
fi
if [ -z "$XAS" ] && [ "`uname -s`" = "HP-UX" ]; then
    for c in /opt/binutils/bin/as /opt/gnu/binutils2461/bin/as; do
        [ -x "$c" ] && { XAS=$c; break; }
    done
    for c in /opt/binutils/bin/ar /opt/gnu/binutils2461/bin/ar; do
        [ -x "$c" ] && { XAR=$c; break; }
    done
fi
if [ -z "$XAS" ] || [ ! -x "$XAS" ] || [ -z "$XAR" ] || [ ! -x "$XAR" ]; then
    echo "SKIP: archive tests need an ia64 as and ar (see tests/local.conf.example)"
    exit 0
fi

rm -rf $W; mkdir -p $W

asm() {        # asm <name> <body-file>
    CHECKS=`expr $CHECKS + 1`
    if $XAS -mlp64 -o $W/$1.o $2 2> $W/as.err; then :; else
        echo "FAIL: assembling $1:"; cat $W/as.err; exit 1
    fi
}

# --- fixtures ------------------------------------------------------------
# used:    referenced by the program, and itself calls into `helper`
# helper:  referenced only by `used`, so it must be pulled transitively
# unused:  referenced by nothing, so it must never be pulled
# seeded:  referenced by nothing either, but named with -u
cat > $W/used.s <<'EOF'
	.text
	.align 16
	.global get_code
get_code:
	alloc	r32 = ar.pfs, 0, 2, 1, 0
	mov	r33 = b0		// making a call clobbers b0; keep our own
	;;
	br.call.sptk.many b0 = helper
	;;
	mov	b0 = r33
	mov	ar.pfs = r32
	;;
	br.ret.sptk.many b0
	;;
EOF
cat > $W/helper.s <<'EOF'
	.text
	.align 16
	.global helper
helper:
	alloc	r32 = ar.pfs, 0, 1, 0, 0
	;;
	mov	r8 = 42
	mov	ar.pfs = r32
	;;
	br.ret.sptk.many b0
	;;
EOF
cat > $W/unused.s <<'EOF'
	.text
	.align 16
	.global never_referenced
never_referenced:
	alloc	r32 = ar.pfs, 0, 1, 0, 0
	;;
	mov	r8 = 99
	mov	ar.pfs = r32
	;;
	br.ret.sptk.many b0
	;;
EOF
cat > $W/seeded.s <<'EOF'
	.text
	.align 16
	.global only_via_u
only_via_u:
	alloc	r32 = ar.pfs, 0, 1, 0, 0
	;;
	mov	r8 = 7
	mov	ar.pfs = r32
	;;
	br.ret.sptk.many b0
	;;
EOF
# a weak reference must NOT drag a member in
cat > $W/weakref.s <<'EOF'
	.text
	.align 16
	.weak	never_referenced
	.global _start
_start:
	alloc	r32 = ar.pfs, 0, 1, 1, 0
	;;
	br.call.sptk.many b0 = get_code
	;;
	mov	r33 = r8
	;;
	br.call.sptk.many b0 = _hld_exit
	;;
EOF

asm used   $W/used.s
asm helper $W/helper.s
asm unused $W/unused.s
asm seeded $W/seeded.s
asm main   $W/weakref.s
asm stub   tests/asm/exit_stub.s

CHECKS=`expr $CHECKS + 1`
if $XAR rcs $W/libtest.a $W/used.o $W/helper.o $W/unused.o $W/seeded.o $W/stub.o \
        2> $W/ar.err; then :; else
    echo "FAIL: creating the archive:"; cat $W/ar.err; exit 1
fi

# --- link ----------------------------------------------------------------
CHECKS=`expr $CHECKS + 1`
if $HLD -e _start -o $W/prog $W/main.o -L$W -ltest 2> $W/link.err; then :; else
    echo "FAIL: linking against the archive:"; cat $W/link.err; exit 1
fi

syms=`$RE -s $W/prog`
has() {
    CHECKS=`expr $CHECKS + 1`
    if printf '%s\n' "$syms" | grep "$2" > /dev/null 2>&1; then :; else
        echo "FAIL: $1"
        FAIL=1
    fi
}
hasnt() {
    CHECKS=`expr $CHECKS + 1`
    if printf '%s\n' "$syms" | grep "$2" > /dev/null 2>&1; then
        echo "FAIL: $1"
        FAIL=1
    fi
}

has   "the referenced member is pulled in"            "get_code"
has   "a member referenced only by another is pulled" "helper"
hasnt "an unreferenced member is left out"            "never_referenced"
hasnt "a member named by nothing is left out"         "only_via_u"

# -u seeds an undefined symbol, which must pull its member
CHECKS=`expr $CHECKS + 1`
if $HLD -e _start -o $W/prog_u -u only_via_u $W/main.o -L$W -ltest \
        2> $W/link.err; then
    if $RE -s $W/prog_u | grep "only_via_u" > /dev/null 2>&1; then :; else
        echo "FAIL: -u did not pull the member defining the symbol"
        FAIL=1
    fi
else
    echo "FAIL: link with -u:"; cat $W/link.err
    FAIL=1
fi

# An archive named before the object that needs it resolves nothing: that is
# what strict left-to-right ordering means, and --start-group is the fix.
CHECKS=`expr $CHECKS + 1`
if $HLD -e _start -o $W/prog_order -L$W -ltest $W/main.o 2> $W/order.err; then
    echo "FAIL: archive named before its user should not have resolved"
    FAIL=1
else
    CHECKS=`expr $CHECKS + 1`
    if grep "undefined symbol" $W/order.err > /dev/null 2>&1; then :; else
        echo "FAIL: expected a clean 'undefined symbol' diagnostic, got:"
        cat $W/order.err
        FAIL=1
    fi
fi

CHECKS=`expr $CHECKS + 1`
if $HLD -e _start -o $W/prog_group --start-group -L$W -ltest $W/main.o --end-group \
        2> $W/group.err; then :; else
    echo "FAIL: --start-group did not resolve across the group:"
    cat $W/group.err
    FAIL=1
fi

# --- run, where that is meaningful --------------------------------------
if [ "`uname -s 2>/dev/null`" = "HP-UX" ] && [ "`uname -m 2>/dev/null`" = "ia64" ]; then
    CHECKS=`expr $CHECKS + 1`
    $W/prog
    rc=$?
    if [ $rc -ne 42 ]; then
        echo "FAIL: archive-linked program exited $rc, expected 42"
        FAIL=1
    fi
    ran="and ran"
else
    ran="(not run: needs HP-UX/ia64)"
fi

# --- --whole-archive takes members nothing references ----------------------
# Embedding a static library INTO a shared library is the job this exists for:
# without it an archive that nothing references contributes nothing, which is
# correct archive semantics and not what that job wants.
CHECKS=`expr $CHECKS + 1`
if $HLD -b +h libwa.so --whole-archive -o $W/wa.so $W/libtest.a 2> $W/link.err; then
    got=`$RE -s $W/wa.so 2>/dev/null \
         | awk '/\.dynsym/{f=1;next} /\.symtab/{f=0} f' | grep -c never_referenced`
    if [ "$got" = "0" ]; then
        echo "FAIL: --whole-archive did not take the unreferenced member"
        FAIL=1
    fi
else
    echo "FAIL: --whole-archive link failed:"; cat $W/link.err; FAIL=1
fi
CHECKS=`expr $CHECKS + 1`
if $HLD -b +h libwa.so --whole-archive --no-whole-archive -o $W/wa2.so $W/libtest.a \
        2> $W/link.err; then
    got=`$RE -s $W/wa2.so 2>/dev/null \
         | awk '/\.dynsym/{f=1;next} /\.symtab/{f=0} f' | grep -c never_referenced`
    if [ "$got" != "0" ]; then
        echo "FAIL: --no-whole-archive did not turn it back off"
        FAIL=1
    fi
fi

if [ $FAIL -eq 0 ]; then
    echo "OK: archive checks passed ($CHECKS checks) $ran"
else
    echo "FAILURES present ($CHECKS checks)"
fi
exit $FAIL
