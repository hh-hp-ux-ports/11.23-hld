// pa_nfd.s -- pa.s as a kernel object: .psr nofuncdesc,absolute gives the
// e_flags HP cc -Wc,-codeconvention,esnonreloc does (0x198).
	.psr	msb,nofuncdesc,absolute
	.radix	C
	.global	bar
	.type	bar,@function
	.section .text = "ax", "progbits"
	.align	16
	.global	foo
	.type	foo,@function
	.proc	foo
foo::
	alloc	r35=ar.pfs,0,4,0,0
	;;
	mov	r34=b0
	;;
	br.call.dptk.many b0=bar
	;;
	br.call.dptk.many b0=lfun
	;;
	mov	ar.pfs=r35
	mov	b0=r34
	;;
	br.ret.sptk.many b0
	.endp	foo
	.proc	lfun
lfun:
	br.ret.sptk.many b0
	.endp	lfun
	.section .data = "aw", "progbits"
	.align	8
	.global	pdat
	.type	pdat,@object
pdat:	data8	foo
	data8	bar
	data8	ldat+4
ldat:	data8	0
	.weak	wk
	.type	wk,@object
wk:	data8	1
	.global	cmn
	.type	cmn,@object
	.common	cmn,16,8
