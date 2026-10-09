// tkm_meta.s -- the KC_META records of the "tkm" fixture module, in the two
// sections kcmodule looks for. Only the shape matters to the tests: data
// and a pointer to it that needs a relocation. HP `as' syntax.
	.psr	msb,nofuncdesc,absolute
	.radix	C
	.section .data.KC_META = "aw", "progbits"
	.align	16
	.global	tkm_metadata
	.type	tkm_metadata,@object
tkm_metadata:
	data4	1
	data4	0
	data8	0
	.section .data.KC_META_PTR = "aw", "progbits"
	.align	8
	.global	tkm_metadata_ptr
	.type	tkm_metadata_ptr,@object
tkm_metadata_ptr:
	data8	tkm_metadata
