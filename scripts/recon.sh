#!/bin/sh
# hld ground-truth recon — run on a real HP-UX 11.23/Itanium LP64 system.
# Produces the link artifacts under samples/gt1/ and the observations written
# up by hand in docs/format-notes.md and docs/link-contract.md.
# Runs from a NON-LOGIN shell: full paths only, POSIX sh only, HP grep only.
#
# Usage: sh recon.sh <working-dir>
# <working-dir> should be empty/scratch; all output lands there.

W=${1:?usage: sh recon.sh <working-dir>}
GCC=/opt/gcc474/bin/gcc
LD=/usr/ccs/bin/ld

mkdir -p "$W"
cd "$W" || exit 1
exec > recon.log 2>&1
set -x
date
uptime
uname -a
# confirm nothing else heavy is using the machine before linking
ps -ef | grep make | grep -v grep
ps -ef | grep cc1 | grep -v grep
ps -ef | grep xgcc | grep -v grep

# --- the linker we are replacing
what $LD
$LD -V
ls -l $LD

# --- library landscape
ls -l /usr/ccs/lib/hpux64 > ls-ccs-lib-hpux64.txt 2>&1
ls -l /usr/lib/hpux64 > ls-usr-lib-hpux64.txt 2>&1
ls /usr/lib/hpux64/*.a > ls-hpux64-archives.txt 2>&1
$GCC -mlp64 -print-file-name=crtbegin.o
$GCC -mlp64 -print-file-name=crtend.o
$GCC -mlp64 -print-search-dirs > gcc-search-dirs.txt 2>&1

# --- test sources -------------------------------------------------------
cat > hello.c <<'EOF'
#include <stdio.h>
int g_init = 42;
int g_bss;
static const char msg[] = "hello-from-hld-groundtruth";
int add3(int a) { return a + 3; }
int main(void) { printf("%s %d %d\n", msg, g_init + add3(1), g_bss); return 0; }
EOF

cat > ret42.c <<'EOF'
int main(void) { return 42; }
EOF

cat > shlib.c <<'EOF'
int exported_var = 7;
int exported_fun(int x) { return x + exported_var; }
EOF

cat > useshlib.c <<'EOF'
extern int exported_fun(int x);
int main(void) { return exported_fun(35); }
EOF

# --- dynamic hello: THE canonical link line capture ---------------------
$GCC -mlp64 -v -save-temps -o hello hello.c 2> gcc-v-link.txt
./hello
echo "hello exit=$?"

# link map from HP ld for layout ground truth
$GCC -mlp64 -o hello_m hello.c -Wl,-m > hello-ldmap.txt 2>&1

# plain LP64 objects at O0 and O2 (relocation variety)
$GCC -mlp64 -c hello.c -o hello_O0.o
$GCC -mlp64 -O2 -c hello.c -o hello_O2.o
$GCC -mlp64 -c ret42.c -o ret42.o

# minimal dynamic exec
$GCC -mlp64 -o ret42 ret42.c
./ret42
echo "ret42 exit=$?"

# --- shared library + consumer -------------------------------------------
# Link with -L. (the current directory) so the embedded RUNPATH (HP ld embeds
# the -L list by default — see docs/format-notes.md) doesn't bake in this
# machine's directory layout.
$GCC -mlp64 -fPIC -c shlib.c -o shlib_pic.o
$GCC -mlp64 -shared -o libshlib.so shlib_pic.o
$GCC -mlp64 -v -o useshlib useshlib.c -L. -lshlib 2> gcc-v-link-shlib.txt
SHLIB_PATH=. LD_LIBRARY_PATH=. ./useshlib
echo "useshlib exit=$? (expect 42)"

# --- static attempt (does hpux64 even have archive libc?) ---------------
$GCC -mlp64 -static -o hello_static hello.c 2> gcc-static-err.txt
if [ -x hello_static ]; then
  ./hello_static
  echo "hello_static exit=$?"
else
  echo "STATIC LINK NOT AVAILABLE"
fi

# --- runtime attributes --------------------------------------------------
/usr/bin/chatr hello > chatr-hello.txt 2>&1
/usr/bin/chatr ret42 > chatr-ret42.txt 2>&1
/usr/bin/chatr libshlib.so > chatr-libshlib.txt 2>&1
/usr/bin/chatr /usr/lib/hpux64/libc.so.1 > chatr-libc.txt 2>&1

# --- system files for offline byte-level study --------------------------
cp /usr/ccs/lib/hpux64/crt0.o sys_crt0.o
cp /usr/lib/hpux64/dld.so sys_dld.so
cp /usr/lib/hpux64/libc.so.1 sys_libc.so.1
cp /usr/lib/hpux64/libdl.so.1 sys_libdl.so.1
cp /usr/lib/hpux64/libunwind.so.1 sys_libunwind.so.1

ls -l
date
echo RECON-COMPLETE
