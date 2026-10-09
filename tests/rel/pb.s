// pb.s -- the other half of pa.s (see there): defines `bar', has a local
// `lfun' and `ldat' of the same names as pa.s's, and a larger COMMON `cmn'.
	.psr	msb
	.radix	C
	.section .text = "ax", "progbits"
	.align	16
	.global	bar
	.type	bar,@function
	.proc	bar
bar::
	br.call.dptk.many b0=lfun
	;;
	br.ret.sptk.many b0
	.endp	bar
	.proc	lfun
lfun:
	br.ret.sptk.many b0
	.endp	lfun
	.section .data = "aw", "progbits"
	.align	16
ldat:	data8	bar
	data8	ldat
	.global	cmn
	.type	cmn,@object
	.common	cmn,32,16
