// km_gnu.s — the code of a module as gcc -mno-pic and GNU as -mauto-pic
// make it (e_flags 0x98: NOFUNCDESC_CONS_GP, but no ABSOLUTE and no
// REDUCEDFP, which GNU as cannot spell). Assemble with
//   ia64-hp-hpux11.23-as -mlp64 -mauto-pic
// It calls into the kernel (PCREL21B), reads its own data by absolute
// address (IMM64), uses only f6-f8 of the floating-point registers, and
// defines the wrapper tkm_shtmod.o points at.

	.text
	.align	16
	.global	km_load
	.type	km_load, @function
km_load:
	alloc	r32 = ar.pfs, 1, 2, 1, 0
	movl	r14 = km_count
	;;
	ld8	r15 = [r14]
	setf.sig f6 = r32
	;;
	setf.sig f7 = r15
	;;
	xma.l	f8 = f6, f7, f0
	;;
	getf.sig r15 = f8
	;;
	st8	[r14] = r15
	mov	r35 = r15
	br.call.sptk.many b0 = printf
	;;
	mov	r8 = 0
	br.ret.sptk.many b0
	;;

	.data
	.align	8
km_count:
	data8	1
	.global	tkm_wrapper
	.type	tkm_wrapper, @object
	.size	tkm_wrapper, 48
tkm_wrapper:
	data4	10
	data4	0
	data8	km_load
	data8	0
	data8	0
	data8	0
	data8	0
