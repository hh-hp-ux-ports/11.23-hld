// hello_static.s — rung 1 of the emulator fixture ladder: a static LP64
// HP-UX/Itanium program that calls write(1, msg, len) and then exit(7).
// No libc, no dynamic loader, no PT_INTERP, no .dynamic — the kernel needs
// none of those (established by field-mutation, docs/format-notes.md), so
// this exercises an ELF64-MSB image and nothing else.
//
// Syscall numbers are MEASURED from HP's own libc.so.1, not assumed:
//   SYS_exit  = 1   (_exit)
//   SYS_write = 4   (_write_sys: `mov r8=4` before the gateway sequence)
// Gateway contract: number in r8, table base in ar.k7, entry =
// ld8 [ar.k7 + 8*number], indirect br.call through b7, link in b6.
//
// Expected behaviour: writes "hld rung1\n" (10 bytes) to fd 1, exits 7.
// A runner that gets the text but the wrong exit code, or the reverse,
// has one half working — the two are deliberately independent signals.

	.text
	.align	16
	.global	_start
_start:
	alloc	r32 = ar.pfs, 0, 1, 3, 0	// 1 local, 3 outputs
	;;
	movl	r34 = msg			// out1 = buffer (absolute: gp is 0
	;;					//   at entry in a static exec)
	mov	r33 = 1				// out0 = fd 1
	mov	r35 = 10			// out2 = length
	;;
	br.call.sptk.many b0 = write_stub
	;;
	alloc	r32 = ar.pfs, 0, 1, 1, 0	// re-shape the frame for exit
	;;
	mov	r33 = 7				// out0 = exit status
	;;
	br.call.sptk.many b0 = exit_stub
	;;
.Lself1:
	br	.Lself1				// not reached
	;;

// No alloc in either stub: with sol = 0 the incoming arguments are this
// frame's outputs too, so the gateway sees them as its own stacked
// registers. This mirrors what libc's own _write_sys/_exit do.
write_stub:
	mov	r31 = ar.k7
	mov	r8 = 4				// SYS_write
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
	br.ret.sptk.many b0
	;;

exit_stub:
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
.Lself2:
	br	.Lself2				// not reached
	;;

	.data
	.align	8
msg:
	.ascii	"hld rung1\n"
