// gp_a.s — reads a short-data variable gp-relatively and exits with it.
// Exercises R_IA64_IMM64 (against the linker-defined __gp) and
// R_IA64_GPREL22, plus the linker's __gp placement.
//
// Note: a STATIC executable is entered with gp == 0 — the kernel does not
// set it up (probed on 11.23; in a dynamic executable the loader sets gp
// from DT_PLTGOT before transferring control). So the program establishes
// gp itself, which is what a startup file does on other platforms.

	.text
	.align	16
	.global	_start
_start:
	alloc	r32 = ar.pfs, 0, 1, 1, 0
	movl	gp = __gp			// R_IA64_IMM64, linker-defined
	;;
	addl	r14 = @gprel(g_val), gp		// R_IA64_GPREL22
	;;
	ld8	r33 = [r14]			// out0 = g_val
	;;
	br.call.sptk.many b0 = _hld_exit
	;;
