// A weak definition. GNU ld's ld-elfweak: a strong definition anywhere in
// the link overrides it, whichever order the objects appear in.
	.data
	.weak  val
	.type  val, @object
	.size  val, 8
val:
	.quad  1
