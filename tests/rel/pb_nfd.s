// pb_nfd.s -- pb.s as a kernel object (e_flags 0x198), see pa_nfd.s.
	.psr	msb,nofuncdesc,absolute
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
