// A tentative definition. GNU ld's ld-elfcomm: when two commons of the same
// name differ in size the LARGEST wins, and a real definition beats both.
	.comm  cval, 8, 8
