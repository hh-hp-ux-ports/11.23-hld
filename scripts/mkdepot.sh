#!/bin/sh
# Package hld as a swinstallable SD-UX depot. Runs on HP-UX, non-root.
#
#   sh scripts/mkdepot.sh [version] [output.depot]
#
# Produces a serial depot file containing one product, HLD, with three
# filesets:
#
#   HLD.RUN    the linker and its inspection tool under /opt/hld/bin, and
#              /opt/hld/bin added to /etc/PATH
#   HLD.LDOVR  an `ld` alias in /opt/hld/override, placed AHEAD of
#              /usr/ccs/bin on /etc/PATH so it takes precedence over the
#              system linker
#   HLD.SRC    the complete corresponding source under /opt/hld/src, so the
#              binaries and the source they were built from travel together
#              (GPLv3 section 6)
#
# A serial depot is the form swpackage can produce without root, and it is
# then gzipped, which is how HP-UX depots are normally handed around. SD's own
# file compression is not usable here: it is rejected for a serial depot and
# for swpackage's default reinstall_files=false, and building the directory
# depot it would require needs root. See docs/packaging.md.

set -e

# src/version.h is the single source of truth; the binary stamps the same
# string into everything it links, so a depot cannot claim a version that
# does not match the linker inside it.
VERSION=${1:-}
if [ -z "$VERSION" ]; then
    VERSION=`sed -n 's/^#define HLD_VERSION "\(.*\)"/\1/p' src/version.h`
fi
if [ -z "$VERSION" ]; then
    echo "mkdepot: cannot read HLD_VERSION from src/version.h" >&2
    exit 1
fi
OUT=${2:-build/hld-$VERSION-ia64-11.23.depot}
STAGE=build/depotstage
PSF=build/hld.psf
PREFIX=/opt/hld
HERE=`pwd`

PATH=/usr/sbin:$PATH
export PATH

if [ "`uname -s 2>/dev/null`" != "HP-UX" ]; then
    echo "mkdepot: SD depots can only be built on HP-UX (this is `uname -s`)" >&2
    echo "         build there:  make package" >&2
    exit 1
fi
if [ ! -x build/hld ]; then
    echo "mkdepot: build/hld is missing — run make first" >&2
    exit 1
fi

rm -rf $STAGE $PSF
mkdir -p $STAGE$PREFIX/bin $STAGE$PREFIX/override $STAGE$PREFIX/src

# --- binaries ------------------------------------------------------------
cp build/hld $STAGE$PREFIX/bin/hld
cp build/hld-readelf $STAGE$PREFIX/bin/hld-readelf
chmod 755 $STAGE$PREFIX/bin/hld $STAGE$PREFIX/bin/hld-readelf

# The override is a symlink, so there is exactly one binary to keep in step.
( cd $STAGE$PREFIX/override && ln -s ../bin/hld ld )

# --- corresponding source ------------------------------------------------
# Everything the project tracks, which is exactly what hld was built from.
# The HP system binaries under samples/sys are deliberately excluded: they
# are not ours to redistribute and are not part of the source.
if [ -d .git ] && command -v git > /dev/null 2>&1; then
    git ls-files > build/srclist.txt
else
    find . -type f \
        | sed 's|^\./||' \
        | grep -v '^build/' \
        | grep -v '^samples/sys/' \
        | grep -v '^\.git' > build/srclist.txt
fi
while read f; do
    d=`dirname "$f"`
    mkdir -p "$STAGE$PREFIX/src/$d"
    cp "$f" "$STAGE$PREFIX/src/$f"
done < build/srclist.txt
cp COPYING $STAGE$PREFIX/COPYING

# --- control scripts -----------------------------------------------------
# They run as root at install time and must be safe to re-run.
mkdir -p $STAGE/ctrl

cat > $STAGE/ctrl/run.postinstall <<'EOF'
#!/bin/sh
# Put /opt/hld/bin on the system PATH (/etc/PATH is read by /etc/profile).
P=/etc/PATH
[ -f $P ] || exit 0
grep -q "/opt/hld/bin" $P && exit 0
cp -p $P $P.pre-hld 2>/dev/null || true
echo "`cat $P`:/opt/hld/bin" > $P
exit 0
EOF

cat > $STAGE/ctrl/run.postremove <<'EOF'
#!/bin/sh
P=/etc/PATH
[ -f $P ] || exit 0
sed -e 's|:/opt/hld/bin||g' -e 's|^/opt/hld/bin:||' $P > $P.new && mv $P.new $P
exit 0
EOF

cat > $STAGE/ctrl/ldovr.postinstall <<'EOF'
#!/bin/sh
# Place /opt/hld/override ahead of /usr/ccs/bin so hld's `ld` is found first.
# Note this governs shell and make invocations of `ld`; gcc locates its
# linker through COMPILER_PATH, so point gcc at hld with -B/opt/hld/override/.
P=/etc/PATH
[ -f $P ] || exit 0
grep -q "/opt/hld/override" $P && exit 0
cp -p $P $P.pre-hld-override 2>/dev/null || true
if grep -q ":/usr/ccs/bin" $P; then
    sed 's|:/usr/ccs/bin|:/opt/hld/override:/usr/ccs/bin|' $P > $P.new
elif grep -q "^/usr/ccs/bin" $P; then
    sed 's|^/usr/ccs/bin|/opt/hld/override:/usr/ccs/bin|' $P > $P.new
else
    echo "/opt/hld/override:`cat $P`" > $P.new
fi
mv $P.new $P
exit 0
EOF

cat > $STAGE/ctrl/ldovr.postremove <<'EOF'
#!/bin/sh
P=/etc/PATH
[ -f $P ] || exit 0
sed -e 's|/opt/hld/override:||g' -e 's|:/opt/hld/override||g' $P > $P.new && mv $P.new $P
exit 0
EOF

chmod 755 $STAGE/ctrl/*

# --- PSF -----------------------------------------------------------------
# There is no recursive file directive in a PSF; every file and symlink gets
# its own line, generated here.
emit_files() {          # $1 = directory to walk, relative to $STAGE$PREFIX
    ( cd $STAGE$PREFIX && find $1 \( -type f -o -type l \) | sed 's|^\./||' ) \
        | while read f; do
            echo "    file $f $f"
        done
}

cat > $PSF <<EOF
vendor
  tag           hh
  title         hld project
end
product
  tag           HLD
  revision      $VERSION
  title         hld — an LP64 linker for HP-UX 11.23 on Itanium
  description   A from-scratch ELF-64 linker for HP-UX/IPF LP64. See /opt/hld/src for the corresponding source and /opt/hld/COPYING for the licence.
  architecture  HP-UX_B.11.23_IA64
  machine_type  ia64*
  os_name       HP-UX
  os_release    ?.11.23
  directory     $PREFIX
  is_locatable  true

  fileset
    tag         RUN
    title       Linker and inspection tool
    revision    $VERSION
    directory   $HERE/$STAGE$PREFIX = $PREFIX
    file_permissions -o bin -g bin
    postinstall $HERE/$STAGE/ctrl/run.postinstall
    postremove  $HERE/$STAGE/ctrl/run.postremove
`emit_files bin`
    file COPYING COPYING
  end

  fileset
    tag         LDOVR
    title       Install as 'ld', ahead of the system linker on PATH
    revision    $VERSION
    directory   $HERE/$STAGE$PREFIX = $PREFIX
    file_permissions -o bin -g bin
    corequisite HLD.RUN
    postinstall $HERE/$STAGE/ctrl/ldovr.postinstall
    postremove  $HERE/$STAGE/ctrl/ldovr.postremove
`emit_files override`
  end

  fileset
    tag         SRC
    title       Complete corresponding source (GPLv3 section 6)
    revision    $VERSION
    directory   $HERE/$STAGE$PREFIX = $PREFIX
    file_permissions -o bin -g bin
`emit_files src`
  end
end
EOF

# --- package -------------------------------------------------------------
rm -f "$OUT"
mkdir -p "`dirname $OUT`"
swpackage -s $PSF -x target_type=tape -d "$OUT" > build/swpackage.log 2>&1 || {
    echo "mkdepot: swpackage failed:" >&2
    tail -20 build/swpackage.log >&2
    exit 1
}

echo "built $OUT"
swlist -s "$HERE/$OUT" 2>/dev/null | sed -n '/HLD/p'

# --- compress ------------------------------------------------------------
# swinstall cannot read a compressed depot, so this is purely for transport
# and storage; it is uncompressed again before installing.
ZIP=
for c in /usr/contrib/bin/gzip /usr/bin/gzip /opt/gnu/bin/gzip; do
    [ -x "$c" ] && { ZIP=$c; break; }
done
if [ -n "$ZIP" ]; then
    rm -f "$OUT.gz"
    if $ZIP -9 "$OUT"; then
        echo "compressed to $OUT.gz"
        OUT="$OUT.gz"
    fi
elif [ -x /usr/bin/compress ]; then
    rm -f "$OUT.Z"
    if /usr/bin/compress "$OUT"; then
        echo "compressed to $OUT.Z"
        OUT="$OUT.Z"
    fi
else
    echo "note: no compressor found, leaving the depot uncompressed"
fi

echo
case "$OUT" in
*.gz|*.Z)
    echo "to install, uncompress first (swinstall reads a plain depot):"
    case "$OUT" in
    *.gz) echo "    $ZIP -dc $HERE/$OUT > /var/tmp/hld.depot" ;;
    *.Z)  echo "    /usr/bin/uncompress -c $HERE/$OUT > /var/tmp/hld.depot" ;;
    esac
    echo "    swinstall -s /var/tmp/hld.depot HLD       # incl. the ld override"
    echo "    swinstall -s /var/tmp/hld.depot HLD.RUN   # linker only"
    ;;
*)
    echo "install as root with:"
    echo "    swinstall -s $HERE/$OUT HLD"
    echo "    swinstall -s $HERE/$OUT HLD.RUN"
    ;;
esac
