	.file	"hello.c"
	.pred.safe_across_calls p1-p5,p16-p63
	.global g_init#
	.section	.sdata,"aws",@progbits
	.align 4
	.type	g_init#, @object
	.size	g_init#, 4
g_init:
	data4	42
	.common	g_bss#,4,4
	.section	.rodata,	"a",	"progbits"
	.align 8
	.type	msg#, @object
	.size	msg#, 27
msg:
	stringz	"hello-from-hld-groundtruth"
	.section	.text,	"ax",	"progbits"
	.align 16
	.global add3#
	.type	add3#, @function
	.proc add3#
add3:
	.prologue 2, 2
	.vframe r2
	mov r2 = r12
	.body
	;;
	st4 [r2] = r32
	ld4 r14 = [r2]
	;;
	adds r14 = 3, r14
	;;
	mov r8 = r14
	.restore sp
	mov r12 = r2
	br.ret.sptk.many b0
	;;
	.endp add3#
	.section	.rodata,	"a",	"progbits"
	.align 8
.LC0:
	stringz	"%s %d %d\n"
	.section	.text,	"ax",	"progbits"
	.align 16
	.global main#
	.type	main#, @function
	.proc main#
main:
	.prologue 14, 32
	.save ar.pfs, r33
	alloc r33 = ar.pfs, 0, 4, 4, 0
	.vframe r34
	mov r34 = r12
	.save rp, r32
	mov r32 = b0
	mov r35 = r1
	.body
	addl r36 = 1, r0
	;;
	br.call.sptk.many b0 = add3#
	mov r1 = r35
	mov r15 = r8
	;;
	addl r14 = @gprel(g_init#), gp
	;;
	ld4 r14 = [r14]
	;;
	add r15 = r15, r14
	addl r14 = @ltoffx(g_bss#), r1
	;;
	ld8.mov r14 = [r14], g_bss#
	;;
	ld4 r14 = [r14]
	addl r36 = @ltoffx(.LC0), r1
	;;
	ld8.mov r36 = [r36], .LC0
	addl r37 = @ltoffx(msg#), r1
	;;
	ld8.mov r37 = [r37], msg#
	mov r38 = r15
	mov r39 = r14
	br.call.sptk.many b0 = printf#
	mov r1 = r35
	mov r14 = r0
	;;
	mov r8 = r14
	mov ar.pfs = r33
	mov b0 = r32
	.restore sp
	mov r12 = r34
	br.ret.sptk.many b0
	;;
	.endp main#
	.global printf#
	.type	printf#, @function
	.ident	"GCC: (GNU) 4.7.4"
