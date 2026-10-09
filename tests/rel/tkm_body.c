/*
 * tkm_body.c -- relocatable-link fixture: the code half of a minimal kernel
 * module ("tkm"), compiled on HP-UX 11.23 with the bundled cc as a kernel
 * object (+DD64 -Wc,-codeconvention,esnonreloc). It is only ever linked by
 * the tests, never loaded. The wrapper mirrors struct modwrapper's shape.
 */
extern int printf(const char *, ...);

struct tkm_wrapper_shape {
    int rev;
    int (*load)(void *);
    int (*unload)(void *);
    void (*halt)(void);
    void *conf;
    void *link;
};

/*
 * Padded past 8 bytes: anything smaller goes to short data and is reached
 * gp-relatively, which a loadable module cannot be (see tkm_sdata.c).
 */
static struct { int n; char pad[28]; } tkm_state;
#define tkm_count tkm_state.n
static long tkm_table[4] = { 1, 2, 3, 4 };

static int tkm_load(void *arg)
{
    tkm_count += (int)tkm_table[tkm_count & 3];
    printf("tkm: load %d %p\n", tkm_count, arg);
    return 0;
}

static int tkm_unload(void *arg)
{
    (void)arg;
    return tkm_count ? 0 : 1;
}

struct tkm_wrapper_shape tkm_wrapper = {
    10, tkm_load, tkm_unload, 0, 0, 0
};
