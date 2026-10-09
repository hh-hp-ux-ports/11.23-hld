/*
 * g1.c -- relocatable-link fixture compiled by gcc 4.7.4 (GNU as), so that
 * hld -r is checked on GNU objects too: short data reached gp-relatively,
 * a linkage-table load (LTOFF22X/LDXMOV), a function pointer (FPTR64MSB)
 * and a COMMON. Its pair is g2.c.
 */
extern int ext_fn(int);
extern int ext_var;
static int counter;
int gdata = 5;
int gbss;
static const char msg[] = "hello";
static int helper(int x) { return x * 3 + counter; }
int g1_entry(int a)
{
    counter += helper(a) + ext_fn(a) + ext_var;
    return counter + msg[a & 3] + gdata;
}
int (*g1_fp)(int) = g1_entry;
