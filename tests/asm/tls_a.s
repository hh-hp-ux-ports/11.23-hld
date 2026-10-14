// Thread-local data next to ordinary data, for the TLS layout check in
// check_link.sh: an initialised and a zero-filled thread-local variable,
// and an ordinary exported variable that must never share their addresses.
	.section .tdata,"awT",@progbits
	.align 8
	.global tinit
tinit:	data8 42
	.section .tbss,"awT",@nobits
	.align 8
	.global tzero
tzero:	.skip 32
	.data
	.align 8
	.global dvar
dvar:	data8 7
