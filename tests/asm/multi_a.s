// multi_a.s — calls a function defined in another object, then exits with
// its return value. Exercises cross-object R_IA64_PCREL21B relocations.

	.text
	.align	16
	.global	_start
_start:
	alloc	r32 = ar.pfs, 0, 1, 1, 0
	;;
	br.call.sptk.many b0 = get_code		// defined in multi_b.s
	;;
	mov	r33 = r8			// out0 = status returned by get_code
	;;
	br.call.sptk.many b0 = _hld_exit	// defined in exit_stub.s
	;;
