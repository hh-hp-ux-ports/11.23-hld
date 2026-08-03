/*
 * stubs.c — long-branch stubs for calls that cannot reach their target.
 *
 * A direct call is R_IA64_PCREL21B: 21 bits of bundle-granular displacement,
 * so +-16 MB from the call. Beyond that the call cannot be encoded at all,
 * and the linker has to route it through a stub near the caller that makes
 * the jump with a wider branch. Text over 16 MB is not exotic — a large C++
 * compiler binary is more than twice that — and mishandling this is one of
 * the defects hld exists to fix, so the reach of every call is checked
 * rather than assumed.
 *
 * The stub is one MLX bundle holding `brl.cond.sptk.many <target>`: a 60-bit
 * displacement, which covers any text this ABI can address. It clobbers no
 * register and does not touch b0, so the call's return address is still the
 * caller's and the target returns straight there. (brl is an Itanium 2
 * instruction; every machine that runs this ABI has it.)
 *
 * The stubs are grouped into islands spread through the executable sections,
 * rather than gathered into one section at the end, which would itself be
 * out of reach from the front of a large text. Islands are sections of their
 * own, inserted between the code sections at intervals well under a branch's
 * reach — code is not all in .text: a C++ compiler emits thousands of
 * one-function .gnu.linkonce.t.* sections, and a call from any of them may
 * need a stub too. A call takes the island nearest to it, so it is always
 * less than one interval away.
 *
 * Sizing has to happen before addresses are final, and inserting the islands
 * moves the addresses that decide which calls need them, so this runs to a
 * fixpoint: place, lay out, scan, repeat until a pass adds nothing. Each
 * pass only adds, so it settles — twice through, in practice.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "ia64_patch.h"

#define U(x) ((unsigned long long)(x))

/*
 * One MLX bundle: nop.m, then brl.cond.sptk.many with a zero displacement,
 * which R_IA64_PCREL60B patches to the real target. Assembled from
 *
 *     { .mlx
 *       nop.m 0
 *       brl.cond.sptk.many <self> ;;
 *     }
 */
static const uint8_t hld_brl_stub[HLD_STUB_BUNDLE] = {
    0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0xc0
};

/*
 * How much code one island serves. A call reaches the nearest island, so
 * this has to stay below the 16 MB a direct branch can encode; the margin
 * absorbs both the stubs' own size and the movement they cause.
 */
#define ZONE_SIZE  (8ULL * 1024 * 1024)

/* Displacement a direct call can encode: 21 bits, bundle-granular, signed. */
#define BRANCH_REACH (1ULL << 24)          /* 2^20 bundles * 16 bytes */

int hld_branch_in_range(uint64_t from, uint64_t to)
{
    uint64_t d = to - from;
    return (int64_t)d >= -(int64_t)BRANCH_REACH
        && (int64_t)d < (int64_t)BRANCH_REACH;
}

static int is_code(const osec *o)
{
    return (o->flags & SHF_ALLOC) && (o->flags & SHF_EXECINSTR)
        && o->type != SHT_NOBITS;
}

/* ---- islands ----------------------------------------------------------- */

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (a < 2) ? v : ((v + a - 1) & ~(a - 1));
}

static uint64_t run_size(const isec *in)
{
    if (in->island) return in->island->size;
    return in->sh ? in->sh->size : 0;
}

static uint64_t run_align(const isec *in)
{
    if (in->island) return 16;
    return (in->sh && in->sh->addralign) ? in->sh->addralign : 1;
}

static uint64_t island_addr(const stubisl *is)
{
    return is->at->out->addr + is->at->out_off;
}

/*
 * Offsets within an output section, once an island has been put in it. A
 * section hld generated itself — the import stubs — has no contributions and
 * its size is not derived from any; recomputing it from nothing would erase
 * it, and every import call would then point past the end of the image.
 */
static void relayout(osec *o)
{
    isec *in;
    uint64_t pos = 0;

    if (!o->first) return;
    for (in = o->first; in; in = in->next) {
        pos = align_up(pos, run_align(in));
        in->out_off = pos;
        pos += run_size(in);
    }
    o->size = pos;
}

static void relayout_code(hld_link *L)
{
    osec *o;
    for (o = L->osecs; o; o = o->next)
        if (is_code(o)) relayout(o);
}

/*
 * Put islands in so that no stretch of code longer than a zone is without
 * one. They go between the contributions making up the code sections, which
 * is the only placement that works both for a compiler emitting thousands of
 * one-function sections and for a single large one. Called before every
 * layout; it only adds, and does nothing once the spacing holds.
 */
/* Splice a new island into `o`'s list ahead of `at` (at the end if NULL). */
static int island_insert(hld_link *L, osec *o, isec *prev, isec *at)
{
    stubisl *is = calloc(1, sizeof *is);
    isec *run = calloc(1, sizeof *run);

    if (!is || !run) { free(is); free(run); return -1; }
    run->out = o;
    run->island = is;
    is->at = run;

    run->next = at;
    if (prev) prev->next = run;
    else o->first = run;
    if (!at) o->tail = &run->next;

    {                                     /* islands in address order */
        stubisl **pp = &L->islands;
        while (*pp) pp = &(*pp)->next;
        *pp = is;
    }
    L->nislands++;
    return 0;
}

static int place_islands(hld_link *L)
{
    osec *o;
    uint64_t since = 0;
    int first = 1, added = 0;

    for (o = L->osecs; o; o = o->next) {
        isec *in, *prev = NULL;

        if (!is_code(o)) continue;
        if (!o->first) { since += o->size; continue; }   /* generated, opaque */
        for (in = o->first; in; prev = in, in = in->next) {
            if (in->island) { since = 0; first = 0; continue; }
            if (first || since + run_size(in) > ZONE_SIZE) {
                if (island_insert(L, o, prev, in) < 0) return -1;
                prev = in;                /* the loop re-sets it anyway */
                since = 0;
                first = 0;
                added = 1;
            }
            since += run_size(in);
        }
        /*
         * And one after the last contribution when the tail of the section
         * has run long. An input section cannot be split, so a single large
         * one — 20 MB of padding inside one object, say — can only be given
         * an island at each end; without the trailing one, a call sitting at
         * its far end has nothing in reach behind it.
         */
        if (since > ZONE_SIZE) {
            isec *last = o->first;
            while (last && last->next) last = last->next;
            if (island_insert(L, o, last, NULL) < 0) return -1;
            since = 0;
            added = 1;
        }
    }
    if (added) relayout_code(L);
    return added;
}

/*
 * The nearest island this call can actually branch to. Nearest alone is not
 * enough: an island can be the closest one and still be out of reach, and a
 * call has to be able to get there.
 */
static stubisl *island_for_call(hld_link *L, uint64_t from)
{
    stubisl *is, *best = NULL;
    uint64_t bestd = 0;

    for (is = L->islands; is; is = is->next) {
        uint64_t a = island_addr(is);
        uint64_t d = a > from ? a - from : from - a;
        if (!hld_branch_in_range(from, a)) continue;
        if (!best || d < bestd) { best = is; bestd = d; }
    }
    return best;
}

/*
 * Any stub for this target that the call can reach will serve. Searching
 * only the nearest island is what stopped this converging: putting a stub in
 * moves every address after it, so the island that is nearest changes
 * between passes, the previous pass's stub is not found where it is looked
 * for, and a fresh one is added every pass. Reuse is what makes the set
 * settle — a call that already has a stub in reach never needs another.
 */
static unsigned stub_hashval(hld_gsym *g, isec *in, uint64_t off)
{
    uintptr_t k = (uintptr_t)g ^ (uintptr_t)in;
    return (unsigned)((k ^ (k >> 16) ^ (uintptr_t)off) % HLD_STUBHASH);
}

/*
 * Any stub for this target that the call can reach will serve -- searching
 * only the nearest island is what stopped this converging. But sweeping every
 * island's every stub to find it is quadratic in stub count, and this runs
 * once per out-of-range branch in the sizing fixpoint and again during
 * relocation. Key on the target; the chain then holds only that target's
 * stubs, one per island that needed one, and the reach test picks among them.
 */
stubent *hld_stub_find(hld_link *L, uint64_t from, hld_gsym *g, isec *in,
                       uint64_t off)
{
    stubent *s;

    for (s = L->stub_hash[stub_hashval(g, in, off)]; s; s = s->hnext)
        if (s->g == g && s->in == in && s->off == off
            && hld_branch_in_range(from, hld_stub_addr(L, s)))
            return s;
    return NULL;
}

static int stub_add(hld_link *L, uint64_t from, hld_gsym *g, isec *in,
                    uint64_t off)
{
    stubisl *is = island_for_call(L, from);
    stubent *s;

    if (!is) return -1;
    s = calloc(1, sizeof *s);
    if (!s) return -1;
    s->g = g;
    s->in = in;
    s->off = off;
    s->slot = is->size;
    s->isl = is;
    is->size += HLD_STUB_BUNDLE;
    if (!is->stail) is->stail = &is->stubs;   /* first stub in this island */
    *is->stail = s;
    is->stail = &s->next;
    {   /* and into the by-target index hld_stub_find() uses */
        unsigned hv = stub_hashval(g, in, off);
        s->hnext = L->stub_hash[hv];
        L->stub_hash[hv] = s;
    }
    L->nstubs++;
    return 0;
}

uint64_t hld_stub_addr(hld_link *L, const stubent *s)
{
    (void)L;
    return island_addr(s->isl) + s->slot;
}

/* ---- the scan ---------------------------------------------------------- */

/*
 * Walk every direct call and add a stub wherever the target is out of reach.
 * Returns the number added, or -1 on failure.
 */
static long scan(hld_link *L)
{
    size_t i;
    uint32_t j;
    long added = 0;

    for (i = 0; i < L->nobjs; i++) {
        hld_elf *e = L->objs[i];
        for (j = 1; j < e->eh.shnum; j++) {
            hld_shdr *rsh = &e->shdrs[j];
            hld_rela *rel;
            hld_sym *syms;
            size_t nrel, nsyms, k;
            char err[HLD_ERRSZ];
            isec *site;

            if (rsh->type != SHT_RELA) continue;
            if (rsh->info == 0 || rsh->info >= e->eh.shnum) continue;
            if (!(e->shdrs[rsh->info].flags & SHF_ALLOC)) continue;
            site = hld_isec_of(L, e, rsh->info);
            if (!site || !is_code(site->out)) continue;
            if (rsh->link >= e->eh.shnum) continue;

            rel = hld_read_relas(e, rsh, &nrel, err);
            if (!rel) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }
            syms = hld_read_syms(e, &e->shdrs[rsh->link], &nsyms, err);
            if (!syms) {
                snprintf(L->err, HLD_ERRSZ, "%s", err);
                free(rel);
                return -1;
            }

            for (k = 0; k < nrel; k++) {
                const hld_rela *r = &rel[k];
                hld_gsym *g;
                isec *in;
                uint64_t off, from, to;
                const char *nm;

                if (r->type != R_IA64_PCREL21B) continue;
                if (hld_reloc_target(L, e, syms, nsyms, r, &g, &in, &off,
                                     &nm) < 0) {
                    free(syms); free(rel);
                    return -1;
                }
                from = site->out->addr + site->out_off + (r->offset & ~3ULL);
                /*
                 * A call to an import goes to the stub the dynamic machinery
                 * placed for it, so that is the address to measure — and it
                 * can be out of reach itself, since the import stubs sit
                 * past the whole of the text. Such a call takes two hops:
                 * one long branch to reach the import stub, which then makes
                 * the indirect jump. Its address is known here even though
                 * the symbol's value is not filled in until later.
                 */
                /*
                 * An exported symbol this library also uses goes through the
                 * same stub, so it is that address the branch has to reach --
                 * hld_relocate() redirects there, and measuring the function
                 * instead decides a call is in range that then is not.
                 */
                if (g && (g->kind == HLD_SYM_IMPORT
                          || (g->has_plt && g->kind == HLD_SYM_DEFINED)))
                    to = L->stubsec ? L->stubsec->addr + g->stub_off : 0;
                else
                    to = g ? g->value + off
                           : (in ? in->out->addr + in->out_off + off : off);
                if (hld_branch_in_range(from, to)) continue;
                if (hld_stub_find(L, from, g, in, off)) continue;
                if (stub_add(L, from, g, in, off) < 0) {
                    snprintf(L->err, HLD_ERRSZ,
                             "no place within reach of 0x%llx to put a "
                             "long-branch stub for `%s' (an input section "
                             "longer than a branch's reach cannot be split)",
                             U(from), nm && nm[0] ? nm : "a local target");
                    free(syms); free(rel);
                    return -1;
                }
                added++;
            }
            free(syms);
            free(rel);
        }
    }
    return added;
}

int hld_alloc_stubs(hld_link *L)
{
    osec *o;
    uint64_t code = 0;
    int pass;

    for (o = L->osecs; o; o = o->next)
        if (is_code(o)) code += o->size;
    /* Nothing can be out of reach until there is more code than the reach. */
    if (code < BRANCH_REACH) return 0;

    for (pass = 0; pass < 8; pass++) {
        long added;

        if (place_islands(L) < 0) {
            snprintf(L->err, HLD_ERRSZ, "out of memory");
            return -1;
        }
        if (hld_layout(L) < 0) return -1;
        added = scan(L);
        if (added < 0) return -1;
        if (added == 0) return 0;
        relayout_code(L);                    /* the islands just grew */
        if (hld_layout(L) < 0) return -1;
    }
    snprintf(L->err, HLD_ERRSZ,
             "long-branch stubs did not settle after 8 passes (%llu stubs)",
             U(L->nstubs));
    return -1;
}

/* Write the stub bundles, once every address is final. */
int hld_write_stubs(hld_link *L)
{
    stubisl *is;
    stubent *s;

    for (is = L->islands; is; is = is->next) {
        if (!is->at->out->data) continue;
        for (s = is->stubs; s; s = s->next) {
            uint64_t at = hld_stub_addr(L, s);
            uint64_t to = s->g ? s->g->value + s->off
                               : (s->in ? s->in->out->addr + s->in->out_off
                                          + s->off
                                        : s->off);
            uint8_t *p = is->at->out->data + is->at->out_off + s->slot;

            memcpy(p, hld_brl_stub, HLD_STUB_BUNDLE);
            if (hld_ia64_install_value(p, 1, to - at, R_IA64_PCREL60B)
                != HLD_PATCH_OK) {
                snprintf(L->err, HLD_ERRSZ,
                         "long branch to %s is out of reach even for brl",
                         s->g ? s->g->name : "a local target");
                return -1;
            }
        }
    }
    return 0;
}

void hld_free_stubs(hld_link *L)
{
    stubisl *is, *isn;

    for (is = L->islands; is; is = isn) {
        stubent *s, *sn;
        isn = is->next;
        for (s = is->stubs; s; s = sn) { sn = s->next; free(s); }
        free(is);   /* the run itself is freed with its output section */
    }
    L->islands = NULL;
}
