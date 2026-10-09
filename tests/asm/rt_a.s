// rt_a.s — round trip through hld -r (check_reloc.sh): reaches its own
// short data gp-relatively and its own data by absolute address, both
// through local labels, so the assembler writes the relocations against
// SECTION symbols plus an addend (R_IA64_GPREL22 .sdata+8, R_IA64_IMM64
// .data+16). -r has to shift exactly those addends when it puts rt_b's
// sections behind these. Adds rt_get()'s answer and exits with the sum, 42.

	.text
	.align	16
	.global	_start
_start:
	alloc	r32 = ar.pfs, 0, 4, 1, 0
	movl	gp = __gp
	;;
	addl	r14 = @gprel(.La_s), gp		// GPREL22 against .sdata + 8
	movl	r15 = .La_d			// IMM64 against .data + 16
	;;
	ld8	r34 = [r14]			// 10
	ld8	r35 = [r15]			// 5
	br.call.sptk.many b0 = rt_get		// 27, from rt_b
	;;
	add	r34 = r34, r35
	;;
	add	r36 = r34, r8			// out0 = 42
	br.call.sptk.many b0 = _hld_exit
	;;

	.sdata
	.align	8
	data8	1000
.La_s:	data8	10

	.data
	.align	8
	data8	2000
	data8	3000
.La_d:	data8	5
