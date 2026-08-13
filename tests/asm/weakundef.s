// A weak reference that nothing defines resolves to zero and must not fail
// the link (GNU ld's ld-undefined weak behaviour).
	.weak  nosuchsym
	.data
	.global wptr
	.type   wptr, @object
	.size   wptr, 8
wptr:
	.quad  nosuchsym
