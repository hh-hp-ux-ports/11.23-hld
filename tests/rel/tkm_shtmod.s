// tkm_shtmod.s -- the .shtmod record of the "tkm" fixture module: 27 bytes,
// version 2, a capacity count, and at offset 16 a pointer to the module's
// wrapper, which is what the loader follows. The assembler can only make
// it PROGBITS; the loader wants SHT_MOD (0x60000001), and hld -r makes it so.
	.psr	msb,nofuncdesc,absolute
	.radix	C
	.global	tkm_wrapper
	.type	tkm_wrapper,@object
	.section .shtmod = "aw", "progbits"
	.align	8
	.global	tkm_shtmod
	.type	tkm_shtmod,@object
tkm_shtmod:
	data4	2
	data4	77
	data8	0
	data8	tkm_wrapper
	data1	0
	data1	0
	data1	0
