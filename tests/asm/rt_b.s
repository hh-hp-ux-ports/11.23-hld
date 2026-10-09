// rt_b.s — the other half of rt_a.s: rt_get() returns 20 + 7, read from its
// own short data and data the same way, at offsets that differ from
// rt_a's so that an unshifted addend reads the wrong word.

	.text
	.align	16
	.global	rt_get
rt_get:
	addl	r14 = @gprel(.Lb_s), gp		// GPREL22 against .sdata + 16
	movl	r15 = .Lb_d			// IMM64 against .data + 8
	;;
	ld8	r14 = [r14]
	ld8	r15 = [r15]
	;;
	add	r8 = r14, r15
	br.ret.sptk.many b0
	;;

	.sdata
	.align	8
	data8	7777
	data8	9999
.Lb_s:	data8	20

	.data
	.align	8
	data8	1
.Lb_d:	data8	7
