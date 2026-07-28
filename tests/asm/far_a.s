// far_a.s — the near end of a call that cannot be encoded as a direct
// branch. Linked with a large filler section in between, `far_target` sits
// more than the 16 MB an R_IA64_PCREL21B can reach, so the call has to be
// routed through a long-branch stub. The far end calls back to
// `near_answer` here, so both directions are exercised.

	.text
	.align	16
	.global	_start
_start:
	alloc	r32 = ar.pfs, 0, 1, 1, 0
	;;
	br.call.sptk.many b0 = far_target	// forward, out of reach
	;;
	mov	r33 = r8			// out0 = status it worked out
	;;
	br.call.sptk.many b0 = _hld_exit	// defined in exit_stub.s
	;;
.Lself:
	br	.Lself				// not reached
	;;

// Called from the far end, so this one is reached by a backward long branch.
	.global	near_answer
near_answer:
	mov	r8 = 42
	br.ret.sptk.many b0
	;;
