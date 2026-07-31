/* mirrors libiberty/hashtab.c:81  --  htab_hash htab_hash_pointer = hash_pointer; */
typedef unsigned (*hashfn_t)(const void *);
static unsigned hash_impl(const void *p){ return (unsigned)(((unsigned long)p)>>3); }
hashfn_t hashvar = hash_impl;      /* GLOBAL function-POINTER VARIABLE, lands in .sdata */
