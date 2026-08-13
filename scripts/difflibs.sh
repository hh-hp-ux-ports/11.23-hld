#!/bin/sh
# Relink real shared libraries with two hld versions and account for every
# byte that differs.
#
#   sh scripts/difflibs.sh <reference-hld> <objdir|archive> [more...]
#
#   sh scripts/difflibs.sh /mnt/debianshare/hld-testbin/hld-0.12.2 \
#       /var/tmp/build-libstdc++/src/.libs \
#       /opt/gcc474/lib/gcc/ia64-hp-hpux11.23/4.7.4/hpux64/libgcc.a
#
# This is where the defects have actually been. Every serious one this linker
# has had was in shared-library output, and the unit tests missed each of them
# because a fixture is a hypothesis about which dimensions matter: the real
# libraries have thousands of symbols, hidden visibility, vtables, static
# initialisers holding imported addresses. Relinking them and diffing against
# the last release is the check that has repeatedly found what the suite could
# not -- 0.12.2 was accepted on exactly this evidence, one differing byte
# inside .comment.
#
# A difference is not automatically a defect and identity is not automatically
# success; the point is that every differing byte is ATTRIBUTED. Two versions
# stamp different strings into .comment, so that difference is expected and is
# reported separately from any other.
#
# Inputs are a directory of .o files or an .a archive. Archives are taken
# whole, since laziness would otherwise make the comparison depend on which
# members happened to be pulled.

REF=$1
[ -n "$REF" ] || {
    echo "usage: sh scripts/difflibs.sh <reference-hld> <objdir|archive> [...]"
    exit 2
}
shift
CUR=${CUR:-build/hld}
RE=${RE:-build/hld-readelf}
# Overridable so two runs can proceed without one erasing the other's
# artefacts -- the whole point of keeping them is to inspect them afterwards.
W=${W:-build/difflibs}

[ -x "$REF" ] || { echo "no reference linker at $REF"; exit 2; }
[ -x "$CUR" ] || { echo "build first: make"; exit 2; }
[ $# -gt 0 ] || { echo "SKIP: no object directories or archives given"; exit 0; }

echo "reference: $REF  (`$REF -V 2>&1 | head -1`)"
echo "current:   $CUR  (`$CUR -V 2>&1 | head -1`)"

rm -rf $W
# Same BASENAME on both sides, different directories. The output name becomes
# the library's SONAME and lands in .dynstr, so linking to <name>.ref.so and
# <name>.cur.so makes the two differ by the three bytes that spell the tag --
# a difference invented entirely by the harness, and one that reads exactly
# like a defect in the linker.
mkdir -p $W/ref $W/cur || exit 1
FAIL=0
NJOB=0

# Build a decimal section table ONCE per library: "start end name", skipping
# NOBITS. Doing the hex conversion and the lookup per differing byte instead
# re-parses several thousand section headers each time -- on a real 31 MB
# libstdc++ with ~9600 sections that turns a second of work into many minutes,
# which is precisely the case this tool exists for.
#
# A NOBITS section occupies NO file space but still carries a nominal
# sh_offset, usually the same one as whatever really holds those bytes.
# Matching it invents findings: .sbss and .comment share an offset, and a
# version-stamp byte got reported as a change to .sbss.
sectab() {
    $RE -S "$1" 2>/dev/null |
      sed 's/^ *\[ *[0-9]*\] *//' |
      while read nm ty addr off sz rest; do
        [ "$ty" = NOBITS ] && continue
        case "$off$sz" in *[!0-9a-fA-F]*|"") continue ;; esac
        o=$((0x$off)); s=$((0x$sz))
        [ "$s" -gt 0 ] || continue
        echo "$o $((o + s)) $nm"
      done > "$2"
}

# Attribute every offset in one pass against that table. cmp reports 1-based
# offsets; the table is 0-based.
attribute() {
    awk 'NR==FNR { lo[NR]=$1; hi[NR]=$2; nm[NR]=$3; n=NR; next }
         { off = $1 - 1
           for (i = 1; i <= n; i++)
               if (off >= lo[i] && off < hi[i]) { print nm[i]; break } }' \
        "$1" -
}

for src in "$@"; do
    NJOB=`expr $NJOB + 1`
    name=`basename "$src" | sed 's/[^A-Za-z0-9_.-]/_/g'`
    if [ -d "$src" ]; then
        objs=`find "$src" -name '*.o' 2>/dev/null | sort`
        [ -n "$objs" ] || { echo "  $name: no .o files — skipped"; continue; }
        inputs="$objs"
    elif [ -f "$src" ]; then
        inputs="--whole-archive $src --no-whole-archive"
    else
        echo "  $name: not found — skipped"
        continue
    fi

    ok=1
    for tag in ref cur; do
        case $tag in ref) LD=$REF ;; cur) LD=$CUR ;; esac
        if $LD -shared -o $W/$tag/$name.so $inputs > $W/$tag/$name.log 2>&1; then :; else
            echo "  $name: the $tag linker refused it — see $W/$tag/$name.log"
            head -2 $W/$tag/$name.log | sed 's/^/      /'
            ok=0
        fi
    done
    #
    # One linker accepting what the other refuses is itself the finding, and a
    # more serious one than any byte difference.
    #
    [ $ok -eq 1 ] || { FAIL=1; continue; }

    if cmp -s $W/ref/$name.so $W/cur/$name.so; then
        echo "  $name: identical (`wc -c < $W/ref/$name.so | tr -d ' '` bytes)"
        continue
    fi

    #
    # Size first. When the two images are different LENGTHS every byte after
    # the first insertion shifts, so a byte-wise diff reports a sea of
    # differences and locating "the first 400" just names whatever section
    # follows the insertion -- on libstdc++ that was .IA_64.unwind_info, which
    # had nothing to do with the change. Compare what is countable instead.
    #
    zr=`wc -c < $W/ref/$name.so | tr -d ' '`
    zc=`wc -c < $W/cur/$name.so | tr -d ' '`
    if [ "$zr" -ne "$zc" ]; then
        echo "  $name: SIZE CHANGED $zr -> $zc (`expr $zc - $zr` bytes)"
        echo "      byte offsets are not comparable across a size change;"
        echo "      dynamic relocations, by type:"
        for t in ref cur; do
            $RE -r $W/$t/$name.so 2>/dev/null | grep R_IA64 |
              awk '{print $3}' | sort | uniq -c | sort -rn |
              awk -v s=$t '{printf "        %-4s %6d %s\n", s, $1, $2}'
        done
        FAIL=1
        continue
    fi

    #
    # ONE capped pass. Two 31 MB libraries that differ widely make `cmp -l'
    # emit millions of lines, and running it twice -- once to count, once to
    # attribute -- was slower than the links themselves. head closes the pipe,
    # so cmp stops at the cap instead of walking the rest of the file. The
    # count is then a floor, and says so rather than pretending to be exact.
    #
    CAP=20000
    cmp -l $W/ref/$name.so $W/cur/$name.so 2>/dev/null |
        head -$CAP > $W/$name.diff
    ndiff=`wc -l < $W/$name.diff | tr -d ' '`
    [ "$ndiff" -ge $CAP ] && ndiff="$CAP+ (capped)"
    sectab $W/ref/$name.so $W/$name.sections
    secs=`head -400 $W/$name.diff | attribute $W/$name.sections |
          sort -u | tr '\n' ' '`
    [ -n "$secs" ] || secs="(outside every section — headers or padding)"
    echo "  $name: $ndiff differing bytes, in: $secs"
    case "$secs" in
    ".comment "|".comment")
        echo "      only the version stamp — the expected difference" ;;
    *)
        echo "      NOT only .comment: this changes the emitted image, so it"
        echo "      needs a reason before it ships"
        FAIL=1 ;;
    esac
done

echo
if [ $FAIL -eq 0 ]; then
    echo "OK: $NJOB library relink(s) accounted for."
    echo "    Identity here means the two linkers agree on these inputs; it is"
    echo "    not evidence about inputs that were not relinked."
    exit 0
fi
echo "DIFFERENCES need attention (see above); artefacts kept in $W"
exit 1
