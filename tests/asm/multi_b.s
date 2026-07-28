// multi_b.s — the callee for multi_a.s. Allocates its own frame so br.ret
// restores the caller's correctly.

	.text
	.align	16
	.global	get_code
get_code:
	alloc	r32 = ar.pfs, 0, 1, 0, 0
	;;
	mov	r8 = 42
	mov	ar.pfs = r32
	;;
	br.ret.sptk.many b0
	;;
