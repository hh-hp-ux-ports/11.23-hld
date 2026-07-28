/*
 * stubs.c — long-branch stubs for calls that cannot reach their target.
 *
 * A direct call is R_IA64_PCREL21B: 21 bits of bundle-granular displacement,
 * so +-16 MB from the call. Beyond that the call cannot be encoded at all,
 * and the linker has to route it through a stub near the caller that makes
 * the jump with a wider branch. Text over 16 MB is not exotic — a large C++
 * compiler binary is more than twice that — and mis-handling this is one of
 * the defects hld exists to fix, so the reach is checked for every call
 * rather than assumed.
 *
 * The stub is one MLX bundle holding `brl.cond.sptk.many <target>`: a 60-bit
 * displacement, which covers any text this ABI can address. It clobbers no
 * register and does not touch b0, so the call's return address is still the
 * caller's and the target returns straight there. (brl is an Itanium 2
 * instruction; every machine that runs this ABI has it.)
 *
 * Stubs live in islands *inside* .text rather than in one section at the end,
 * because a single trailing section is itself unreachable from the front of a
 * large text. An island is placed at the head of every zone of text, and a
 * call takes the island in its own zone: at most one zone away, hence always
 * in range. The zone is deliberately well under the branch's reach, so the
 * address movement caused by inserting the islands cannot invalidate the
 * choice.
 *
 * Sizing has to happen before addresses are final, and inserting stubs moves
 * the addresses that decide which calls need them, so this runs to a
 * fixpoint: scan, insert, lay out again, rescan until a pass adds nothing.
 * Each pass only adds, so it terminates.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "ia64_patch.h"

#define U(x) ((unsigned long long)(x))

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (a < 2) ? v : ((v + a - 1) & ~(a - 1));
}

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
 * How much text one island serves. A call reaches back at most this far, so
 * it must stay below the 16 MB a PCREL21B can encode; the margin absorbs the
 * shift caused by inserting the islands themselves.
 */
#define ZONE_SIZE  (12ULL * 1024 * 1024)

/* Displacement a direct call can encode: 21 bits, bundle-granular, signed. */
#define BRANCH_REACH (1ULL << 24)          /* 2^20 bundles * 16 bytes */

int hld_branch_in_range(uint64_t from, uint64_t to)
{
    uint64_t d = to - from;
    return (int64_t)d >= -(int64_t)BRANCH_REACH
        && (int64_t)d < (int64_t)BRANCH_REACH;
}

/* ---- islands ----------------------------------------------------------- */

static stubisl *island_for(hld_link *L, uint64_t zone)
{
    stubisl *is;

    for (is = L->islands; is; is = is->next)
        if (is->zone == zone) return is;

    is = calloc(1, sizeof *is);
    if (!is) return NULL;
    is->zone = zone;
    /* keep the list ordered by zone; the rebuild walks it in place order */
    {
        stubisl **pp = &L->islands;
        while (*pp && (*pp)->zone < zone) pp = &(*pp)->next;
        is->next = *pp;
        *pp = is;
    }
    return is;
}

/*
 * The stub a call at `from` must use to reach `g`/`in`+`off`. Both the
 * allocation pass and relocation ask this, so they cannot disagree.
 */
stubent *hld_stub_find(hld_link *L, uint64_t from, hld_gsym *g, isec *in,
                       uint64_t off)
{
    uint64_t zone;
    stubisl *is;
    stubent *s;

    if (from < L->text_addr) return NULL;
    zone = (from - L->text_addr) / ZONE_SIZE;
    for (is = L->islands; is; is = is->next) {
        if (is->zone != zone) continue;
        for (s = is->stubs; s; s = s->next)
            if (s->g == g && s->in == in && s->off == off)
                return s;
        return NULL;
    }
    return NULL;
}

static int stub_add(hld_link *L, uint64_t from, hld_gsym *g, isec *in,
                    uint64_t off)
{
    uint64_t zone = (from - L->text_addr) / ZONE_SIZE;
    stubisl *is = island_for(L, zone);
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
    {                                     /* append, so slots stay ordered */
        stubent **pp = &is->stubs;
        while (*pp) pp = &(*pp)->next;
        *pp = s;
    }
    L->nstubs++;
    return 0;
}

/*
 * Re-lay the text section with the islands in place. Offsets within an
 * output section are otherwise assigned as inputs arrive; here they are
 * recomputed from scratch, inserting each island at the head of its zone.
 */
static void place_islands(hld_link *L)
{
    osec *o = L->textsec;
    stubisl *is;
    isec *in;
    uint64_t pos = 0;

    if (!o) return;
    is = L->islands;
    for (in = o->first; in; in = in->next) {
        uint64_t align = (in->sh && in->sh->addralign) ? in->sh->addralign : 1;
        uint64_t sz = in->sh ? in->sh->size : 0;

        /*
         * Islands go in before the first input section that starts at or
         * past the zone boundary, so every call in the zone has one behind
         * it. Several can land in the same gap if a single input section
         * spans more than one zone.
         */
        while (is && pos >= is->zone * ZONE_SIZE) {
            pos = align_up(pos, 16);
            is->out_off = pos;
            pos += is->size;
            is = is->next;
        }
        pos = align_up(pos, align);
        in->out_off = pos;
        pos += sz;
    }
    while (is) {                          /* any island past the last input */
        pos = align_up(pos, 16);
        is->out_off = pos;
        pos += is->size;
        is = is->next;
    }
    o->size = pos;
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
            if (!site || site->out != L->textsec) continue;
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
                if (g && g->kind == HLD_SYM_IMPORT)
                    to = L->stubsec ? L->stubsec->addr + g->stub_off : 0;
                else
                    to = g ? g->value + off
                           : (in ? in->out->addr + in->out_off + off : off);
                if (hld_branch_in_range(from, to)) continue;
                if (hld_stub_find(L, from, g, in, off)) continue;
                if (stub_add(L, from, g, in, off) < 0) {
                    snprintf(L->err, HLD_ERRSZ, "out of memory");
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
    int pass;

    L->textsec = osec_find_pub(L, ".text");
    if (!L->textsec) return 0;
    /* Nothing can be out of reach until the text itself is bigger than one. */
    if (L->textsec->size < BRANCH_REACH) return 0;

    for (pass = 0; pass < 8; pass++) {
        long added = scan(L);
        if (added < 0) return -1;
        if (added == 0) return 0;
        place_islands(L);
        if (hld_layout(L) < 0) return -1;
    }
    snprintf(L->err, HLD_ERRSZ,
             "long-branch stubs did not settle after 8 passes (%llu stubs)",
             U(L->nstubs));
    return -1;
}

uint64_t hld_stub_addr(hld_link *L, const stubent *s)
{
    return L->textsec->addr + s->isl->out_off + s->slot;
}

/* Write the stub bundles, once every address is final. */
int hld_write_stubs(hld_link *L)
{
    stubisl *is;
    stubent *s;

    if (!L->textsec || !L->textsec->data) return 0;

    for (is = L->islands; is; is = is->next)
        for (s = is->stubs; s; s = s->next) {
            uint64_t at = hld_stub_addr(L, s);
            uint64_t to = s->g ? s->g->value + s->off
                               : (s->in ? s->in->out->addr + s->in->out_off
                                          + s->off
                                        : s->off);
            uint8_t *p = L->textsec->data + is->out_off + s->slot;

            memcpy(p, hld_brl_stub, HLD_STUB_BUNDLE);
            if (hld_ia64_install_value(p, 1, to - at, R_IA64_PCREL60B)
                != HLD_PATCH_OK) {
                snprintf(L->err, HLD_ERRSZ,
                         "long branch to %s is out of reach even for brl",
                         s->g ? s->g->name : "a local target");
                return -1;
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
        free(is);
    }
    L->islands = NULL;
}
