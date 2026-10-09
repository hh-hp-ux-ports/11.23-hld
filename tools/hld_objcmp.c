/*
 * hld-objcmp -- compare two relocatable objects by what they MEAN, not by
 * their bytes.
 *
 * Two linkers combining the same inputs will number sections and symbols
 * differently and lay the file out differently, and none of that matters
 * to whoever links or loads the result. What matters -- the contract -- is:
 *
 *   - e_flags, type, class;
 *   - each section, BY NAME: type, flags, size, alignment, entsize, what its
 *     sh_link/sh_info refer to (again by name), and its contents;
 *   - each section's relocations, as a set of (offset, type, target,
 *     addend), where the target is a global symbol's name or, for anything
 *     local, the output section plus offset it resolves to -- so a
 *     reference through a local symbol and one through its section symbol
 *     compare equal, as they are for the loader;
 *   - the global symbols, by name, and the local ones as a multiset.
 *
 * Built for checking `hld -r' against HP ld -r (docs/kernel-modules.md), and
 * usable on any two ET_REL objects. Exit status 0 when nothing differs.
 *
 *   hld-objcmp [-x SECTION]... [-q] A.o B.o
 *
 * -x leaves a section (and its relocations) out of the comparison; -q prints
 * only the count.
 */
#include "../src/port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/elf64.h"

#define U(x)  ((unsigned long long)(x))

static const char *excl[64];
static int nexcl, quiet;
static unsigned long ndiff;
static const char *nameA = "A", *nameB = "B";

static void diff(const char *fmt, const char *a, const char *b, const char *c)
{
    ndiff++;
    if (quiet) return;
    printf(fmt, a ? a : "", b ? b : "", c ? c : "");
    putchar('\n');
}

static int excluded(const char *name)
{
    int i;
    for (i = 0; i < nexcl; i++)
        if (strcmp(excl[i], name) == 0) return 1;
    return 0;
}

static int skip_type(uint32_t t)
{
    return t == SHT_NULL || t == SHT_SYMTAB || t == SHT_STRTAB || t == SHT_RELA;
}

static const hld_shdr *sec_by_name(const hld_elf *e, const char *name)
{
    uint32_t j;
    for (j = 1; j < e->eh.shnum; j++)
        if (!skip_type(e->shdrs[j].type) && strcmp(e->shdrs[j].name, name) == 0)
            return &e->shdrs[j];
    return NULL;
}

static const char *sec_name_of(const hld_elf *e, uint32_t idx)
{
    if (idx == 0) return "-";
    if (idx >= e->eh.shnum) return "?";
    return e->shdrs[idx].name;
}

static const char *shndx_name(const hld_elf *e, uint16_t s)
{
    switch (s) {
    case SHN_UNDEF: return "UND";
    case SHN_ABS: return "ABS";
    case SHN_COMMON: return "COM";
    case SHN_IA_64_ANSI_COMMON: return "ANSI_COM";
    default: return sec_name_of(e, s);
    }
}

static const hld_shdr *symtab_of(const hld_elf *e)
{
    uint32_t j;
    for (j = 1; j < e->eh.shnum; j++)
        if (e->shdrs[j].type == SHT_SYMTAB) return &e->shdrs[j];
    return NULL;
}

/* ---- header ------------------------------------------------------------ */

static void cmp_header(const hld_elf *a, const hld_elf *b)
{
    char x[64], y[64];

    if (a->eh.cls != b->eh.cls || a->eh.data != b->eh.data
        || a->eh.osabi != b->eh.osabi || a->eh.abiver != b->eh.abiver) {
        snprintf(x, sizeof x, "%u/%u/%u/%u", a->eh.cls, a->eh.data,
                 a->eh.osabi, a->eh.abiver);
        snprintf(y, sizeof y, "%u/%u/%u/%u", b->eh.cls, b->eh.data,
                 b->eh.osabi, b->eh.abiver);
        diff("header: class/data/osabi/abiversion %s vs %s", x, y, NULL);
    }
    if (a->eh.type != b->eh.type || a->eh.machine != b->eh.machine) {
        snprintf(x, sizeof x, "%u/%u", a->eh.type, a->eh.machine);
        snprintf(y, sizeof y, "%u/%u", b->eh.type, b->eh.machine);
        diff("header: type/machine %s vs %s", x, y, NULL);
    }
    if (a->eh.flags != b->eh.flags) {
        snprintf(x, sizeof x, "0x%x", a->eh.flags);
        snprintf(y, sizeof y, "0x%x", b->eh.flags);
        diff("header: e_flags %s vs %s", x, y, NULL);
    }
}

/* ---- sections ---------------------------------------------------------- */

static void cmp_one_section(const hld_elf *a, const hld_shdr *sa,
                            const hld_elf *b, const hld_shdr *sb)
{
    char x[96], y[96];
    const char *n = sa->name;

#define FIELD(label, fa, fb) \
    if ((fa) != (fb)) { \
        snprintf(x, sizeof x, "0x%llx", U(fa)); \
        snprintf(y, sizeof y, "0x%llx", U(fb)); \
        diff("section %s: " label " %s vs %s", n, x, y); \
    }
    FIELD("type", sa->type, sb->type)
    FIELD("flags", sa->flags, sb->flags)
    FIELD("size", sa->size, sb->size)
    FIELD("align", sa->addralign, sb->addralign)
    FIELD("entsize", sa->entsize, sb->entsize)
#undef FIELD
    if (strcmp(sec_name_of(a, sa->link), sec_name_of(b, sb->link)) != 0) {
        snprintf(x, sizeof x, "%s", sec_name_of(a, sa->link));
        snprintf(y, sizeof y, "%s", sec_name_of(b, sb->link));
        diff("section %s: sh_link names %s vs %s", n, x, y);
    }
    if (strcmp(sec_name_of(a, sa->info), sec_name_of(b, sb->info)) != 0) {
        snprintf(x, sizeof x, "%s", sec_name_of(a, sa->info));
        snprintf(y, sizeof y, "%s", sec_name_of(b, sb->info));
        diff("section %s: sh_info names %s vs %s", n, x, y);
    }
    if (sa->type != SHT_NOBITS && sb->type != SHT_NOBITS
        && sa->size == sb->size && sa->size) {
        char err[HLD_ERRSZ];
        const uint8_t *pa = hld_sec_data(a, sa, err);
        const uint8_t *pb = hld_sec_data(b, sb, err);
        if (!pa || !pb) {
            diff("section %s: contents unreadable", n, NULL, NULL);
        } else if (memcmp(pa, pb, (size_t)sa->size) != 0) {
            uint64_t k = 0;
            while (pa[k] == pb[k]) k++;
            snprintf(x, sizeof x, "0x%llx", U(k));
            diff("section %s: contents differ, first at offset %s", n, x, NULL);
        }
    }
}

static void cmp_sections(const hld_elf *a, const hld_elf *b)
{
    uint32_t j;

    for (j = 1; j < a->eh.shnum; j++) {
        const hld_shdr *sa = &a->shdrs[j], *sb;
        if (skip_type(sa->type) || excluded(sa->name)) continue;
        sb = sec_by_name(b, sa->name);
        if (!sb) { diff("section %s: only in %s", sa->name, nameA, NULL); continue; }
        cmp_one_section(a, sa, b, sb);
    }
    for (j = 1; j < b->eh.shnum; j++) {
        const hld_shdr *sb = &b->shdrs[j];
        if (skip_type(sb->type) || excluded(sb->name)) continue;
        if (!sec_by_name(a, sb->name))
            diff("section %s: only in %s", sb->name, nameB, NULL);
    }
}

/* ---- relocations ------------------------------------------------------- */

typedef struct {
    uint64_t off;
    uint32_t type;
    char tgt[160];            /* "sym NAME" or "sec NAME" */
    int64_t add;
} crel;

static int crel_cmp(const void *x, const void *y)
{
    const crel *p = x, *q = y;
    int c;
    if (p->off != q->off) return p->off < q->off ? -1 : 1;
    if (p->type != q->type) return p->type < q->type ? -1 : 1;
    c = strcmp(p->tgt, q->tgt);
    if (c) return c;
    return p->add < q->add ? -1 : p->add > q->add ? 1 : 0;
}

/* The canonical form of every relocation applying to section `name'. */
static crel *canon_relocs(const hld_elf *e, const char *name, size_t *count)
{
    uint32_t j;
    crel *out = NULL;
    size_t n = 0, cap = 0;
    char err[HLD_ERRSZ];

    for (j = 1; j < e->eh.shnum; j++) {
        const hld_shdr *rs = &e->shdrs[j];
        hld_rela *r;
        hld_sym *syms;
        size_t nr, ns, k;

        if (rs->type != SHT_RELA || rs->info >= e->eh.shnum) continue;
        if (strcmp(e->shdrs[rs->info].name, name) != 0) continue;
        if (rs->link >= e->eh.shnum) continue;
        r = hld_read_relas(e, rs, &nr, err);
        syms = hld_read_syms(e, &e->shdrs[rs->link], &ns, err);
        if (!r || !syms) { free(r); free(syms); continue; }
        for (k = 0; k < nr; k++) {
            crel *c;
            if (n == cap) {
                size_t nc = cap ? cap * 2 : 64;
                crel *nb = realloc(out, nc * sizeof *nb);
                if (!nb) break;
                out = nb;
                cap = nc;
            }
            c = &out[n++];
            c->off = r[k].offset;
            c->type = r[k].type;
            c->add = r[k].addend;
            if (r[k].sym == 0 || r[k].sym >= ns) {
                snprintf(c->tgt, sizeof c->tgt, "none");
            } else {
                const hld_sym *s = &syms[r[k].sym];
                if (ELF64_ST_BIND(s->info) != STB_LOCAL) {
                    snprintf(c->tgt, sizeof c->tgt, "sym %s", s->name);
                } else {
                    /* a local resolves to where it is, whatever it is called */
                    snprintf(c->tgt, sizeof c->tgt, "sec %s",
                             shndx_name(e, s->shndx));
                    c->add += (int64_t)s->value;
                }
            }
        }
        free(r);
        free(syms);
    }
    if (n) qsort(out, n, sizeof *out, crel_cmp);
    *count = n;
    return out;
}

static void crel_str(char *buf, size_t sz, const crel *c)
{
    const char *tn = hld_reloc_name(c->type);
    snprintf(buf, sz, "+0x%llx %s %s %+lld", U(c->off), tn ? tn : "?",
             c->tgt, (long long)c->add);
}

static void cmp_relocs(const hld_elf *a, const hld_elf *b)
{
    uint32_t j;

    for (j = 1; j < a->eh.shnum; j++) {
        const hld_shdr *sa = &a->shdrs[j];
        crel *ra, *rb;
        size_t na, nb, ia = 0, ib = 0;
        unsigned shown = 0;
        char x[256], cnt[64];

        if (skip_type(sa->type) || excluded(sa->name)) continue;
        if (!sec_by_name(b, sa->name)) continue;     /* reported already */
        ra = canon_relocs(a, sa->name, &na);
        rb = canon_relocs(b, sa->name, &nb);
        if (na != nb) {
            snprintf(cnt, sizeof cnt, "%lu vs %lu", (unsigned long)na,
                     (unsigned long)nb);
            diff("relocs %s: count %s", sa->name, cnt, NULL);
        }
        while (ia < na || ib < nb) {
            int c = ia >= na ? 1 : ib >= nb ? -1 : crel_cmp(&ra[ia], &rb[ib]);
            if (c == 0) { ia++; ib++; continue; }
            if (c < 0) crel_str(x, sizeof x, &ra[ia++]);
            else crel_str(x, sizeof x, &rb[ib++]);
            if (shown++ < 8)
                diff("relocs %s: only in %s: %s", sa->name,
                     c < 0 ? nameA : nameB, x);
            else
                ndiff++;
        }
        free(ra);
        free(rb);
    }
}

/* ---- symbols ----------------------------------------------------------- */

typedef struct {
    char key[256];            /* name, for globals */
    char desc[320];           /* everything else that matters */
} csym;

static int csym_cmp(const void *x, const void *y)
{
    const csym *p = x, *q = y;
    int c = strcmp(p->key, q->key);
    return c ? c : strcmp(p->desc, q->desc);
}

static const char *bind_name(uint8_t b)
{
    return b == STB_LOCAL ? "LOCAL" : b == STB_GLOBAL ? "GLOBAL"
         : b == STB_WEAK ? "WEAK" : "?";
}

static const char *type_name(uint8_t t)
{
    static const char *const n[] = { "NOTYPE", "OBJECT", "FUNC", "SECTION",
                                     "FILE", "COMMON", "TLS" };
    return t < 7 ? n[t] : "?";
}

static csym *canon_syms(const hld_elf *e, int global, size_t *count)
{
    const hld_shdr *st = symtab_of(e);
    hld_sym *syms;
    size_t n, k, m = 0;
    csym *out;
    char err[HLD_ERRSZ];

    *count = 0;
    if (!st) return NULL;
    syms = hld_read_syms(e, st, &n, err);
    if (!syms) return NULL;
    out = calloc(n ? n : 1, sizeof *out);
    if (!out) { free(syms); return NULL; }
    for (k = 1; k < n; k++) {
        const hld_sym *s = &syms[k];
        uint8_t bind = ELF64_ST_BIND(s->info), type = ELF64_ST_TYPE(s->info);
        csym *c;

        if ((bind != STB_LOCAL) != global) continue;
        if (type == STT_SECTION) continue;      /* bookkeeping, not meaning */
        c = &out[m++];
        snprintf(c->key, sizeof c->key, "%s", s->name);
        snprintf(c->desc, sizeof c->desc, "%s %s vis%u %s value 0x%llx size 0x%llx",
                 bind_name(bind), type_name(type),
                 (unsigned)ELF64_ST_VISIBILITY(s->other),
                 shndx_name(e, s->shndx), U(s->value), U(s->size));
    }
    free(syms);
    if (m) qsort(out, m, sizeof *out, csym_cmp);
    *count = m;
    return out;
}

static void cmp_syms(const hld_elf *a, const hld_elf *b, int global)
{
    size_t na, nb, ia = 0, ib = 0;
    csym *sa = canon_syms(a, global, &na);
    csym *sb = canon_syms(b, global, &nb);
    const char *what = global ? "symbol" : "local symbol";
    char x[700];

    while (ia < na || ib < nb) {
        int c;
        if (ia >= na) c = 1;
        else if (ib >= nb) c = -1;
        else c = strcmp(sa[ia].key, sb[ib].key);
        if (c == 0 && strcmp(sa[ia].desc, sb[ib].desc) == 0) { ia++; ib++; continue; }
        if (c == 0 && global) {
            snprintf(x, sizeof x, "%s | %s", sa[ia].desc, sb[ib].desc);
            diff("%s %s: %s", what, sa[ia].key, x);
            ia++; ib++;
            continue;
        }
        if (c == 0) c = strcmp(sa[ia].desc, sb[ib].desc);
        if (c < 0) {
            snprintf(x, sizeof x, "%s: %s", nameA, sa[ia].desc);
            diff("%s %s: only in %s", what, sa[ia].key, x);
            ia++;
        } else {
            snprintf(x, sizeof x, "%s: %s", nameB, sb[ib].desc);
            diff("%s %s: only in %s", what, sb[ib].key, x);
            ib++;
        }
    }
    free(sa);
    free(sb);
}

int main(int argc, char **argv)
{
    hld_elf *a, *b;
    char err[HLD_ERRSZ];
    int i;

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "-x") == 0 && i + 1 < argc && nexcl < 64) {
            excl[nexcl++] = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0) {
            quiet = 1;
        } else {
            fprintf(stderr, "usage: hld-objcmp [-x SECTION]... [-q] A.o B.o\n");
            return 2;
        }
    }
    if (argc - i != 2) {
        fprintf(stderr, "usage: hld-objcmp [-x SECTION]... [-q] A.o B.o\n");
        return 2;
    }
    nameA = argv[i];
    nameB = argv[i + 1];
    a = hld_elf_load(nameA, err);
    if (!a) { fprintf(stderr, "hld-objcmp: %s\n", err); return 2; }
    b = hld_elf_load(nameB, err);
    if (!b) { fprintf(stderr, "hld-objcmp: %s\n", err); hld_elf_free(a); return 2; }

    cmp_header(a, b);
    cmp_sections(a, b);
    cmp_relocs(a, b);
    cmp_syms(a, b, 1);
    cmp_syms(a, b, 0);

    if (ndiff) printf("%lu difference(s)\n", ndiff);
    else printf("no contract differences\n");
    hld_elf_free(a);
    hld_elf_free(b);
    return ndiff ? 1 : 0;
}
