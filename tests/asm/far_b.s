// far_b.s — the far end. Reached only through a long-branch stub, and calls
// straight back across the same distance for the answer it returns.

	.text
	.align	16
	.global	far_target
far_target:
	alloc	r32 = ar.pfs, 0, 2, 0, 0
	mov	r33 = b0
	;;
	br.call.sptk.many b0 = near_answer	// backward, out of reach
	;;
	mov	b0 = r33
	mov	ar.pfs = r32
	;;
	br.ret.sptk.many b0			// returns near_answer's value
	;;
