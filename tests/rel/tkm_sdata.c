/*
 * tkm_sdata.c -- tkm_body.c as a module is easily written by mistake: a
 * small static variable, which the compiler puts in short data and reaches
 * gp-relatively (GPREL22) even when compiling for the kernel. A loadable
 * module has no gp of its own; --kernel-module must refuse this.
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

static int tkm_count;
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
