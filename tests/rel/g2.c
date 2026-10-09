/*
 * g2.c -- the other half of g1.c. Its divisions become millicode calls,
 * which a loadable kernel module may not make: --kernel-module must say so.
 */
extern int g1_entry(int);
int ext_var = 7;
int ext_fn(int v) { return g1_entry(v - 1) / (v | 1); }
long g2_long(long a, long b) { return a % b; }
