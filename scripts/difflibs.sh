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
W=build/difflibs

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

# Which section holds this file offset? Sections are listed with hex offset
# and size; HP's awk has no strtonum, so the arithmetic is done by the shell.
# Takes cmp's 1-BASED offset and converts here: doing it at the call site
# would nest command substitutions, which the older shells mis-parse.
section_at() {
    _want=`expr $2 - 1`
    $RE -S "$1" 2>/dev/null |
      sed 's/^ *\[ *[0-9]*\] *//' |
      while read nm ty addr off sz rest; do
        case "$off$sz" in *[!0-9a-fA-F]*|"") continue ;; esac
        o=$((0x$off)); s=$((0x$sz))
        if [ "$_want" -ge "$o" ] && [ "$_want" -lt $((o + s)) ]; then
            echo "$nm"
            return
        fi
      done
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

    # Attribute the differing bytes. Only the first few hundred are located --
    # enough to name the sections involved without walking a 30 MB library.
    ndiff=`cmp -l $W/ref/$name.so $W/cur/$name.so 2>/dev/null | wc -l | tr -d ' '`
    secs=`cmp -l $W/ref/$name.so $W/cur/$name.so 2>/dev/null | head -400 |
          while read off a b; do
              section_at $W/ref/$name.so $off
          done | sort -u | tr '\n' ' '`
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
