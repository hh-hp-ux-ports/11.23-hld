// exit42.s — the smallest possible HP-UX LP64/Itanium program: _exit(42)
// straight through the kernel syscall gateway, no libc, no dynamic loader.
//
// Gateway contract (docs/format-notes.md, "Startup contract"): syscall
// number in r8 (SYS_exit = 1), gateway table base in ar.k7, entry =
// ld8 [ar.k7 + 8*number], indirect br.call through b7 with the return
// link in b6. Arguments travel per the C ABI: the callee's frame is the
// caller's output area, so a stub that does no alloc passes its own
// incoming argument register straight through to the gateway.

	.text
	.align	16
	.global	_start
_start:
	alloc	r32 = ar.pfs, 0, 1, 1, 0	// 1 local, 1 output
	mov	r33 = 42			// out0 = exit status
	;;
	br.call.sptk.many b0 = exit_stub
	;;
.Lself1:
	br	.Lself1				// not reached
	;;

// Mirrors libc's _exit sequence. No alloc: with sol = 0 the single
// incoming register (the status) is this frame's output too, so the
// gateway sees it as its own first stacked register.
exit_stub:
	mov	r31 = ar.k7
	mov	r8 = 1				// SYS_exit
	;;
	shladd	r31 = r8, 3, r31
	;;
	ld8	r31 = [r31]
	mov	r9 = 2				// libc's _exit sets this; replicate
	;;
	mov	b7 = r31
	;;
	br.call.sptk.few b6 = b7
	;;
.Lself2:
	br	.Lself2				// not reached
	;;
