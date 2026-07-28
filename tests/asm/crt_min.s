// Minimal startup: call main, then hand its result to the C library's exit
// (which flushes buffered output, as returning from main normally would).
	.text
	.align	16
	.global	_start
_start:
	alloc	r32 = ar.pfs, 0, 1, 1, 0
	movl	gp = __gp
	;;
	br.call.sptk.many b0 = main
	;;
	mov	r33 = r8
	;;
	br.call.sptk.many b0 = exit
	;;
