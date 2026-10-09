// pa.s -- relocatable-link fixture, HP `as' syntax (assembled with
// `as +DD64' on HP-UX 11.23; GNU as cannot read it). A global function
// calling an undefined one and a local one, data words naming a global, an
// undefined symbol and a local plus an addend, a weak definition and a
// COMMON. Its pair is pb.s, which defines `bar' and its own local `lfun'.
	.psr	msb
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
