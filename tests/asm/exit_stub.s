// exit_stub.s — _hld_exit(status): terminate via the kernel syscall gateway.
// Shared by the assembly link fixtures. Mirrors libc's own _exit sequence
// (see docs/format-notes.md): number in r8, gateway table at ar.k7, indirect
// call through b7 with the link in b6.
//
// No alloc: with the caller's output area as this frame, the status the
// caller placed in its first output register is what the gateway sees.

	.text
	.align	16
	.global	_hld_exit
_hld_exit:
	mov	r31 = ar.k7
	mov	r8 = 1				// SYS_exit
	;;
	shladd	r31 = r8, 3, r31
	;;
	ld8	r31 = [r31]
	mov	r9 = 2
	;;
	mov	b7 = r31
	;;
	br.call.sptk.few b6 = b7
	;;
.Lhang:
	br	.Lhang				// not reached
	;;
