// pdup.s -- defines `pdat' again, which pa.s already defines strongly:
// combining the two must fail.
	.psr	msb
	.section .data = "aw", "progbits"
	.global	pdat
pdat:	data8	0
