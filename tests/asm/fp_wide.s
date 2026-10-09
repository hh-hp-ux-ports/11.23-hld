// fp_wide.s — code that breaks the reduced floating-point model 11.31's
// kernel objects claim (EF_IA_64_REDUCEDFP: f0, f1 and f6-f11 only). One
// offending instruction per instruction format that names an FR, nine in
// all, each using a register outside the set in a different field; the
// lines marked `ok' stay inside it and must not be reported.
// Assemble with: ia64-hp-hpux11.23-as -mlp64 -mauto-pic

	.text
	.align	16
	.global	fp_wide
	.type	fp_wide, @function
fp_wide:
	fma.d	f12 = f6, f7, f8		// F1, f1 field
	;;
	fma.d	f6 = f6, f7, f40		// F1, f2 field
	;;
	fcmp.eq	p6, p7 = f6, f13		// F4
	;;
	frcpa.s0 f6, p6 = f14, f7		// F6
	;;
	fmerge.s f2 = f6, f7			// F9, f2-f5 are outside too
	;;
	ldf8	f32 = [r14]			// M1 load
	;;
	stf8	[r14] = f33			// M9 store
	;;
	setf.sig f15 = r15			// M18
	;;
	getf.sig r15 = f16			// M19
	;;
	fma.d	f11 = f10, f9, f1		// ok
	;;
	ldf8	f6 = [r14]			// ok
	;;
	fclass.m p6, p7 = f7, 0x1		// ok
	;;
	br.ret.sptk.many b0
	;;
