typedef unsigned (*hashfn_t)(const void *);
static unsigned hash_impl(const void *p){ return (unsigned)(((unsigned long)p)>>3); }
/* 8 MB of initialised data BEFORE the pointer, to push it out of GP-relative reach */
unsigned long pad_before[1024*1024] = { 1 };
hashfn_t hashvar = hash_impl;
unsigned long pad_after[1024*1024] = { 2 };
