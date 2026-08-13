// The same .gnu.linkonce section in two objects. gcc emits one per template
// instantiation and per vtable, so a C++ link sees many copies of each.
	.section .gnu.linkonce.d.dup,"aw",@progbits
	.weak  dupsym
	.type  dupsym, @object
	.size  dupsym, 8
dupsym:
	.quad  11
