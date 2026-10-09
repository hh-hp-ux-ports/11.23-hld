#!/bin/sh
# mkref.sh -- regenerate the relocatable-link fixtures and HP ld -r's
# answers for them. Runs ON HP-UX 11.23/Itanium (needs HP's cc, as and ld,
# and gcc 4.7.4); compiles and links only.
#
#   sh tests/rel/mkref.sh [OUTDIR]       then copy OUTDIR/*.o to tests/rel/obj
#
# The inputs are hld's own fixtures; the r_*.o files are what the
# platform's linker makes of them, and check_reloc.sh holds hld -r to them.
set -e
SRC=`cd \`dirname $0\` && pwd`
OUT=${1:-$SRC/obj}
mkdir -p $OUT
cd $OUT
PATH=/usr/ccs/bin:/usr/bin:$PATH
GCC=${GCC:-/opt/gnu/gcc474/bin/gcc}
KFLAGS="+DD64 -Wc,-codeconvention,esnonreloc"

for f in pa pb pa_nfd pb_nfd pa_rfp pdup tkm_meta tkm_shtmod; do
    as +DD64 -o $f.o $SRC/$f.s
done
cc $KFLAGS -c -o tkm_body.o $SRC/tkm_body.c
cc $KFLAGS -c -o tkm_sdata.o $SRC/tkm_sdata.c
cc $KFLAGS -c -o h2k.o $SRC/g2.c
cc +DD64 -c -o h2.o $SRC/g2.c
$GCC -mlp64 -O2 -c -o g1.o $SRC/g1.c
$GCC -mlp64 -O2 -c -o g2.o $SRC/g2.c
$GCC -mlp64 -O2 -ffunction-sections -fdata-sections -c -o g1fs.o $SRC/g1.c
$GCC -mlp64 -O2 -ffunction-sections -c -o g2fs.o $SRC/g2.c

ld -r -o r_pab.o pa.o pb.o
ld -r -o r_nfd.o pa_nfd.o pb_nfd.o
ld -r -z -o r_z.o pa.o
ld -r -o r_g.o g1.o g2.o
ld -r -o r_gfs.o g1fs.o g2fs.o
ld -r -o r_mix.o g1.o h2.o
ld -r -o r_tkm.o tkm_body.o tkm_meta.o tkm_shtmod.o
rm -f libpb.a
ar rc libpb.a pb.o
ld -r -o r_ar.o pa.o libpb.a
rm -f libpb.a
ls -l
