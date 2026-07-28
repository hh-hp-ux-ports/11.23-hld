/*
 * link.c — section collection, symbol resolution, address assignment and
 * relocation application.
 *
 * Scope note: the static-executable path. Archives and dynamic output are
 * not here yet, so relocations that need them are rejected loudly rather
 * than mis-applied.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "ia64_patch.h"

#define U(x) ((unsigned long long)(x))

static void lerr(hld_link *L, const char *fmt, const char *a, const char *b)
{
    snprintf(L->err, HLD_ERRSZ, fmt, a ? a : "", b ? b : "");
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (a < 2) ? v : ((v + a - 1) & ~(a - 1));
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---- inputs ------------------------------------------------------------ */

int hld_add_object(hld_link *L, const char *path)
{
    hld_elf *e;
    char err[HLD_ERRSZ];
    FILE *probe;

    /* An archive named directly on the command line is searched in place. */
    probe = fopen(path, "rb");
    if (probe) {
        char magic[8];
        size_t got = fread(magic, 1, sizeof magic, probe);
        fclose(probe);
        if (got == sizeof magic && memcmp(magic, "!<arch>\n", 8) == 0) {
            hld_archive *ar;
            if (hld_archive_open(L, path, &ar) < 0) return -1;
            return hld_archive_search(L, ar, NULL);
        }
    }

    e = hld_elf_load(path, err);
    if (!e) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }
    if (e->eh.type == ET_DYN) {
        /* a shared library named directly, as `hld foo.so` */
        hld_elf_free(e);
        return hld_add_dso(L, path);
    }
    if (e->eh.type != ET_REL) {
        lerr(L, "%s: neither an object nor a shared library", path, NULL);
        hld_elf_free(e);
        return -1;
    }
    if (e->eh.machine != EM_IA_64) {
        lerr(L, "%s: not an IA-64 object", path, NULL);
        hld_elf_free(e);
        return -1;
    }
    if (!(e->eh.flags & EF_IA_64_ABI64)) {
        lerr(L, "%s: ILP32 object — hld links LP64 only, and nothing else is "
                "supported yet", path, NULL);
        hld_elf_free(e);
        return -1;
    }
    return hld_input_object(L, e);
}

/* ---- output sections --------------------------------------------------- */

static osec *osec_find(hld_link *L, const char *name)
{
    osec *o;
    for (o = L->osecs; o; o = o->next)
        if (strcmp(o->name, name) == 0)
            return o;
    return NULL;
}

osec *osec_get(hld_link *L, const char *name, uint32_t type, uint64_t flags)
{
    osec *o = osec_find(L, name);

    if (o) {
        /* NOBITS + PROGBITS with the same name: PROGBITS wins the type. */
        if (o->type == SHT_NOBITS && type != SHT_NOBITS)
            o->type = type;
        o->flags |= flags;
        return o;
    }
    o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->name = name;
    o->type = type;
    o->flags = flags;
    o->align = 1;
    o->tail = &o->first;
    *L->osec_tail = o;
    L->osec_tail = &o->next;
    L->nosecs++;
    return o;
}

/*
 * Output order. Within a segment, sections keep the order they are first
 * seen except that NOBITS is forced last (it has no file image). The text
 * segment holds alloc sections without SHF_WRITE, the data segment the rest.
 */
static int sec_is_text(uint64_t flags)
{
    return (flags & SHF_ALLOC) && !(flags & SHF_WRITE);
}

static int collect_sections_of(hld_link *L, hld_elf *e)
{
    uint32_t j;

    {
        for (j = 1; j < e->eh.shnum; j++) {
            hld_shdr *sh = &e->shdrs[j];
            osec *o;
            isec *in;

            if (!(sh->flags & SHF_ALLOC))
                continue;   /* debug/comment/reloc/symtab: not laid out */
            if (sh->type == SHT_GROUP)
                continue;

            o = osec_get(L, sh->name, sh->type, sh->flags);
            if (!o) { lerr(L, "out of memory", NULL, NULL); return -1; }
            if (sh->addralign > o->align) o->align = sh->addralign;
            if (sh->entsize && !o->entsize) o->entsize = sh->entsize;

            in = calloc(1, sizeof *in);
            if (!in) { lerr(L, "out of memory", NULL, NULL); return -1; }
            in->obj = e;
            in->idx = j;
            in->sh = sh;
            in->out = o;
            /* place this contribution at the current end of the output */
            in->out_off = align_up(o->size, sh->addralign ? sh->addralign : 1);
            o->size = in->out_off + sh->size;
            *o->tail = in;
            o->tail = &in->next;
        }
    }
    return 0;
}

/* ---- symbols ----------------------------------------------------------- */

static unsigned symhash(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h % HLD_SYMHASH;
}

hld_gsym *hld_sym_lookup(hld_link *L, const char *name)
{
    hld_gsym *g;
    for (g = L->hash[symhash(name)]; g; g = g->next)
        if (strcmp(g->name, name) == 0)
            return g;
    return NULL;
}

static hld_gsym *sym_intern(hld_link *L, const char *name)
{
    unsigned h = symhash(name);
    hld_gsym *g = hld_sym_lookup(L, name);

    if (g) return g;
    g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->name = xstrdup(name);
    if (!g->name) { free(g); return NULL; }
    g->kind = HLD_SYM_UNDEF;
    g->next = L->hash[h];
    L->hash[h] = g;
    return g;
}

/* Find the input-section record for (obj, shndx). */
static isec *isec_of(hld_link *L, hld_elf *obj, uint32_t shndx)
{
    osec *o;
    isec *in;

    for (o = L->osecs; o; o = o->next)
        for (in = o->first; in; in = in->next)
            if (in->obj == obj && in->idx == shndx)
                return in;
    return NULL;
}

static int resolve_symbols_of(hld_link *L, hld_elf *e)
{
    uint32_t j;
    hld_gsym *g;

    {
        for (j = 1; j < e->eh.shnum; j++) {
            hld_sym *syms;
            size_t n, k;
            char err[HLD_ERRSZ];

            if (e->shdrs[j].type != SHT_SYMTAB) continue;
            syms = hld_read_syms(e, &e->shdrs[j], &n, err);
            if (!syms) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }

            for (k = 0; k < n; k++) {
                hld_sym *s = &syms[k];
                uint8_t bind = ELF64_ST_BIND(s->info);
                uint8_t type = ELF64_ST_TYPE(s->info);

                if (bind == STB_LOCAL) continue;
                if (!s->name[0]) continue;

                g = sym_intern(L, s->name);
                if (!g) { lerr(L, "out of memory", NULL, NULL); free(syms); return -1; }

                if (s->shndx == SHN_UNDEF) {
                    if (g->kind == HLD_SYM_UNDEF && bind == STB_WEAK)
                        g->bind = STB_WEAK;
                    continue;
                }
                if (s->shndx == SHN_COMMON
                    || s->shndx == SHN_IA_64_ANSI_COMMON) {
                    if (g->kind == HLD_SYM_UNDEF
                        || (g->kind == HLD_SYM_COMMON && s->size > g->size)) {
                        g->kind = HLD_SYM_COMMON;
                        g->short_common =
                            (s->shndx == SHN_IA_64_ANSI_COMMON);
                        g->size = s->size;
                        g->value = s->value ? s->value : 8; /* alignment */
                        g->type = type;
                        g->bind = bind;
                        g->obj = e;
                    }
                    continue;
                }
                /* a real definition */
                if (g->kind == HLD_SYM_DEFINED || g->kind == HLD_SYM_ABS) {
                    if (bind == STB_WEAK) continue;       /* keep the strong one */
                    if (g->bind != STB_WEAK) {
                        lerr(L, "duplicate definition of `%s'", g->name, NULL);
                        free(syms);
                        return -1;
                    }
                }
                if (s->shndx == SHN_ABS) {
                    g->kind = HLD_SYM_ABS;
                    g->value = s->value;
                } else {
                    isec *in = isec_of(L, e, s->shndx);
                    if (!in) continue;   /* defined in a non-alloc section */
                    g->kind = HLD_SYM_DEFINED;
                    g->in = in;
                    g->in_off = s->value;
                }
                g->size = s->size;
                g->type = type;
                g->bind = bind;
                g->other = s->other;
                g->obj = e;
            }
            free(syms);
        }
    }

    return 0;
}

/*
 * Bring one object into the link: its sections become output contributions
 * and its symbols enter the table. Inputs are processed in command-line
 * order, so that an archive searched later sees exactly the symbols that are
 * still undefined at its position.
 */
int hld_input_object(hld_link *L, hld_elf *e)
{
    if (L->nobjs == L->objs_cap) {
        size_t nc = L->objs_cap ? L->objs_cap * 2 : 8;
        hld_elf **na = realloc(L->objs, nc * sizeof *na);
        if (!na) { lerr(L, "out of memory", NULL, NULL); return -1; }
        L->objs = na;
        L->objs_cap = nc;
    }
    L->objs[L->nobjs++] = e;

    if (collect_sections_of(L, e) < 0) return -1;
    if (resolve_symbols_of(L, e) < 0) return -1;
    return 0;
}

/* Seed an undefined symbol, as -u does, so it can drive archive extraction. */
int hld_add_undefined(hld_link *L, const char *name)
{
    hld_gsym *g = sym_intern(L, name);

    if (!g) { lerr(L, "out of memory", NULL, NULL); return -1; }
    return 0;
}

/*
 * Tentative definitions become real storage only once every input has been
 * seen: a later object (or an extracted archive member) may yet provide a
 * real definition that this one must not displace.
 */
int hld_allocate_commons(hld_link *L)
{
    hld_gsym *g;
    unsigned h;

    for (h = 0; h < HLD_SYMHASH; h++) {
        for (g = L->hash[h]; g; g = g->next) {
            osec *bss;
            isec *in;

            if (g->kind != HLD_SYM_COMMON) continue;
            bss = g->short_common
                ? osec_get(L, ".sbss", SHT_NOBITS,
                           SHF_ALLOC | SHF_WRITE | SHF_IA_64_SHORT)
                : osec_get(L, ".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE);
            if (!bss) { lerr(L, "out of memory", NULL, NULL); return -1; }
            if (g->value > bss->align) bss->align = g->value;
            in = calloc(1, sizeof *in);
            if (!in) { lerr(L, "out of memory", NULL, NULL); return -1; }
            in->obj = g->obj;
            in->idx = 0;                 /* synthetic: no input section */
            in->sh = NULL;
            in->out = bss;
            in->out_off = align_up(bss->size, g->value ? g->value : 8);
            bss->size = in->out_off + g->size;
            *bss->tail = in;
            bss->tail = &in->next;
            g->kind = HLD_SYM_DEFINED;
            g->in = in;
            g->in_off = 0;
        }
    }
    return 0;
}

/* ---- layout ------------------------------------------------------------ */

/*
 * The symbols the linker itself provides. They are interned before the
 * dynamic tables are sized (so they can be exported — the C library binds
 * to `_end`) and given their values once addresses are known.
 */
static const char *const hld_linker_symbols[] = {
    "__text_start", "__text_start_f", "_etext", "_etext_f",
    "__data_start", "_edata", "_end", "__gp",
    /*
     * The C library binds to these in the executable, so they must exist and
     * be exported even when the program uses no threads: the platform's own
     * linker defines exactly this set.
     */
    "__init_start", "__init_end", "__fini_start", "__fini_end",
    "__hp_preinit_start", "__hp_preinit_end",
    "__TLS_SIZE", "__TLS_INIT_SIZE", "__TLS_INIT_START", "__TLS_INIT_A",
    "__TLS_PREALLOC_DTV_A", "__SYSTEM_ID", "__profil_size",
    /* Segment markers and the dynamic-section address the loader looks up. */
    "_DYNAMIC", "__text_seg", "__data_seg", "__thread_specific_seg",
    NULL
};

int hld_predefine_symbols(hld_link *L)
{
    const char *const *n;

    for (n = hld_linker_symbols; *n; n++) {
        hld_gsym *g = sym_intern(L, *n);
        if (!g) { lerr(L, "out of memory", NULL, NULL); return -1; }
        if (g->kind == HLD_SYM_UNDEF) {
            g->kind = HLD_SYM_ABS;
            g->bind = STB_GLOBAL;
            g->value = 0;                  /* set during layout */
        }
    }
    return 0;
}

static int def_abs(hld_link *L, const char *name, uint64_t val)
{
    hld_gsym *g = sym_intern(L, name);
    if (!g) { lerr(L, "out of memory", NULL, NULL); return -1; }
    if (g->kind == HLD_SYM_DEFINED) return 0;  /* an object defined it: respect that */
    g->kind = HLD_SYM_ABS;
    g->value = val;
    if (!g->bind) g->bind = STB_GLOBAL;
    return 0;
}

/*
 * Assign addresses. The text segment is mapped from file offset 0 so the ELF
 * header and program headers share its first page (as HP ld does); the data
 * segment starts at the next HLD_SEG_ALIGN boundary, keeping file offset
 * congruent to vaddr modulo that alignment.
 */
int hld_layout(hld_link *L)
{
    uint64_t addr, off;
    osec *o;
    hld_gsym *g;
    unsigned h;
    /* PHDR + LOAD text + LOAD data, plus INTERP and DYNAMIC when dynamic */
    uint32_t nphdr = L->dynamic ? 5 : 3;

    L->nphdr = nphdr;

    /* text segment */
    off = (uint64_t)EHDR64_SIZE + (uint64_t)nphdr * PHDR64_SIZE;
    addr = HLD_TEXT_BASE + off;
    L->text_addr = HLD_TEXT_BASE;

    for (o = L->osecs; o; o = o->next) {
        if (!sec_is_text(o->flags)) continue;
        if (o->type == SHT_NOBITS) continue;    /* no NOBITS in text */
        addr = align_up(addr, o->align);
        off = align_up(off, o->align);
        o->addr = addr;
        o->off = off;
        addr += o->size;
        off += o->size;
    }
    L->text_end = addr;
    L->text_filesz = off;

    /*
     * Data segment: file offset congruent to vaddr mod HLD_SEG_ALIGN.
     *
     * Short-addressable sections (.plt/.dlt/.sdata/.sbss) are grouped in the
     * middle so a single gp value reaches all of them through the +-2MB addl
     * window, which is the layout HP's linker uses as well:
     *     [ long PROGBITS ] [ short PROGBITS ] [ short NOBITS ] [ long NOBITS ]
     *                       ^ gp anchors here
     */
    off = align_up(off, HLD_SEG_ALIGN);
    addr = HLD_DATA_BASE;
    L->data_addr = addr;
    L->data_off = off;

#define IS_DATA(o) (!sec_is_text((o)->flags) && ((o)->flags & SHF_ALLOC))
#define IS_SHORT(o) (((o)->flags & SHF_IA_64_SHORT) != 0)

    for (o = L->osecs; o; o = o->next) {          /* long PROGBITS */
        if (!IS_DATA(o) || IS_SHORT(o) || o->type == SHT_NOBITS) continue;
        addr = align_up(addr, o->align);
        off = align_up(off, o->align);
        o->addr = addr; o->off = off;
        addr += o->size; off += o->size;
    }

    /*
     * The linkage tables come first, then gp, then ordinary short data —
     * the arrangement the platform's linker uses, which keeps the tables at
     * negative offsets from gp and leaves the positive half for data.
     */
    for (o = L->osecs; o; o = o->next) {          /* .plt and .dlt first */
        if (!IS_DATA(o) || !IS_SHORT(o) || o->type == SHT_NOBITS) continue;
        if (o != L->pltsec && o != L->dltsec) continue;
        addr = align_up(addr, o->align);
        off = align_up(off, o->align);
        o->addr = addr; o->off = off;
        addr += o->size; off += o->size;
    }

    L->gp = addr;                                 /* anchor past the tables */

    for (o = L->osecs; o; o = o->next) {          /* remaining short PROGBITS */
        if (!IS_DATA(o) || !IS_SHORT(o) || o->type == SHT_NOBITS) continue;
        if (o == L->pltsec || o == L->dltsec) continue;
        addr = align_up(addr, o->align);
        off = align_up(off, o->align);
        o->addr = addr; o->off = off;
        addr += o->size; off += o->size;
    }
    L->data_filesz = off - L->data_off;

    for (o = L->osecs; o; o = o->next) {          /* short NOBITS */
        if (!IS_DATA(o) || !IS_SHORT(o) || o->type != SHT_NOBITS) continue;
        addr = align_up(addr, o->align);
        o->addr = addr; o->off = off;
        addr += o->size;
    }
    for (o = L->osecs; o; o = o->next) {          /* long NOBITS */
        if (!IS_DATA(o) || IS_SHORT(o) || o->type != SHT_NOBITS) continue;
        addr = align_up(addr, o->align);
        o->addr = addr; o->off = off;
        addr += o->size;
    }
    L->data_memsz = addr - L->data_addr;

#undef IS_DATA
#undef IS_SHORT

    /* Linker-defined symbols (docs/format-notes.md). */
    if (def_abs(L, "__text_start", L->text_addr) < 0) return -1;
    if (def_abs(L, "__text_start_f", L->text_addr) < 0) return -1;
    if (def_abs(L, "_etext", L->text_end) < 0) return -1;
    if (def_abs(L, "_etext_f", L->text_end) < 0) return -1;
    if (def_abs(L, "__data_start", L->data_addr) < 0) return -1;
    if (def_abs(L, "_edata", L->data_addr + L->data_filesz) < 0) return -1;
    if (def_abs(L, "_end", L->data_addr + L->data_memsz) < 0) return -1;
    if (def_abs(L, "__gp", L->gp) < 0) return -1;
    /*
     * Array bounds and thread-local descriptors. With no such sections
     * present these collapse to the start of the data segment and to zero
     * sizes, which is what an image with no initializers or TLS wants.
     */
    {
        struct { const char *first, *last, *sec; } bounds[] = {
            { "__init_start", "__init_end", ".init_array" },
            { "__fini_start", "__fini_end", ".fini_array" },
            { "__hp_preinit_start", "__hp_preinit_end", ".HP.preinit" }
        };
        size_t bi;
        for (bi = 0; bi < sizeof bounds / sizeof bounds[0]; bi++) {
            osec *so = osec_find(L, bounds[bi].sec);
            uint64_t lo = so ? so->addr : L->data_addr;
            uint64_t hi = so ? so->addr + so->size : L->data_addr;
            if (def_abs(L, bounds[bi].first, lo) < 0) return -1;
            if (def_abs(L, bounds[bi].last, hi) < 0) return -1;
        }
    }
    if (def_abs(L, "__TLS_SIZE", 0) < 0) return -1;
    if (def_abs(L, "__TLS_INIT_SIZE", 0) < 0) return -1;
    if (def_abs(L, "__TLS_INIT_START", 0) < 0) return -1;
    if (def_abs(L, "__TLS_INIT_A", 0) < 0) return -1;
    if (def_abs(L, "__TLS_PREALLOC_DTV_A", 0) < 0) return -1;
    if (def_abs(L, "__SYSTEM_ID", 0) < 0) return -1;
    if (def_abs(L, "__profil_size", 0x10) < 0) return -1;
    if (def_abs(L, "__text_seg", L->text_addr) < 0) return -1;
    if (def_abs(L, "__data_seg", L->data_addr) < 0) return -1;
    {
        osec *tls = osec_find(L, ".tbss");
        if (def_abs(L, "__thread_specific_seg",
                    tls ? tls->addr : L->data_addr + L->data_memsz) < 0)
            return -1;
    }
    if (L->dynamicsec
        && def_abs(L, "_DYNAMIC", L->dynamicsec->addr) < 0) return -1;

    /* Final addresses for section-relative symbols. */
    for (h = 0; h < HLD_SYMHASH; h++)
        for (g = L->hash[h]; g; g = g->next)
            if (g->kind == HLD_SYM_DEFINED && g->in)
                g->value = g->in->out->addr + g->in->out_off + g->in_off;

    /*
     * Number the output sections now: dynamic symbols reference them by
     * index and are built before the section headers are written.
     */
    {
        uint32_t ndx = 1;                       /* 0 is the null section */
        osec *so;
        for (so = L->osecs; so; so = so->next) so->shndx = ndx++;
    }

    /* Entry point. */
    g = hld_sym_lookup(L, L->entry_name);
    if (!g || g->kind == HLD_SYM_UNDEF) {
        lerr(L, "entry symbol `%s' is not defined", L->entry_name, NULL);
        return -1;
    }
    L->entry = g->value;

    /* Undefined symbols are fatal in a static link. */
    for (h = 0; h < HLD_SYMHASH; h++)
        for (g = L->hash[h]; g; g = g->next)
            if (g->kind == HLD_SYM_UNDEF && g->bind != STB_WEAK) {
                lerr(L, "undefined symbol `%s'", g->name, NULL);
                return -1;
            }
    return 0;
}

/* ---- relocation -------------------------------------------------------- */

/*
 * Identify what a relocation refers to, without needing final addresses:
 * either a global symbol (*gp_out) or a local input section (*in_out) plus
 * an offset. This runs both before layout (to allocate linkage-table slots)
 * and after it (to compute values), so it must not depend on addresses.
 */
static int reloc_target(hld_link *L, hld_elf *e, hld_sym *syms, size_t nsyms,
                        const hld_rela *r, hld_gsym **gp_out, isec **in_out,
                        uint64_t *off_out, const char **name)
{
    hld_sym *s;
    isec *in;
    hld_gsym *g;

    *gp_out = NULL;
    *in_out = NULL;
    *off_out = (uint64_t)r->addend;
    *name = "";

    if (r->sym == 0) { *name = "<none>"; return 0; }
    if (r->sym >= nsyms) {
        lerr(L, "%s: relocation references symbol out of range", e->path, NULL);
        return -1;
    }
    s = &syms[r->sym];
    *name = s->name;

    if (ELF64_ST_BIND(s->info) != STB_LOCAL && s->name[0]) {
        g = hld_sym_lookup(L, s->name);
        if (g && (g->kind == HLD_SYM_DEFINED || g->kind == HLD_SYM_ABS
                  || g->kind == HLD_SYM_IMPORT)) {
            *gp_out = g;      /* an import's value is its call stub */
            return 0;
        }
        if (g && g->bind == STB_WEAK) return 0;   /* undefined weak: 0 */
        lerr(L, "undefined symbol `%s'", s->name, NULL);
        return -1;
    }
    /* local: section-relative */
    if (s->shndx == SHN_ABS) { *off_out += s->value; return 0; }
    if (s->shndx >= e->eh.shnum) {
        lerr(L, "%s: bad section index in symbol", e->path, NULL);
        return -1;
    }
    in = isec_of(L, e, s->shndx);
    if (!in) {
        lerr(L, "%s: relocation against non-allocated section `%s'",
             e->path, e->shdrs[s->shndx].name);
        return -1;
    }
    *in_out = in;
    *off_out += s->value;
    return 0;
}

/* Final address of a resolved target. Valid only after layout. */
static uint64_t target_addr(hld_gsym *g, isec *in, uint64_t off)
{
    if (g) return g->value + off;
    if (in) return in->out->addr + in->out_off + off;
    return off;                     /* absolute, or undefined weak (0) */
}

/* ---- linkage tables (DLT / function descriptors) ----------------------- */

static unsigned lnk_hash(hld_gsym *g, isec *in, uint64_t off)
{
    uintptr_t k = (uintptr_t)g ^ (uintptr_t)in;
    return (unsigned)((k ^ (k >> 16) ^ (uintptr_t)off) % HLD_LNKHASH);
}

static lnkent *lnk_get(hld_link *L, lnkent **hash, lnkent ***tail,
                       lnkent **head, uint64_t *count, uint64_t entsize,
                       hld_gsym *g, isec *in, uint64_t off, int is_fptr)
{
    unsigned h = lnk_hash(g, in, off);
    lnkent *l;

    (void)L;
    for (l = hash[h]; l; l = l->hnext)
        if (l->g == g && l->in == in && l->off == off && l->is_fptr == is_fptr)
            return l;
    l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->g = g;
    l->in = in;
    l->off = off;
    l->is_fptr = is_fptr;
    l->slot = *count * entsize;
    (*count)++;
    l->hnext = hash[h];
    hash[h] = l;
    if (!*head) { *head = l; *tail = &l->next; }
    else { **tail = l; *tail = &l->next; }
    return l;
}

static lnkent *dlt_get(hld_link *L, hld_gsym *g, isec *in, uint64_t off,
                       int is_fptr)
{
    return lnk_get(L, L->dlt_hash, &L->dlt_tail, &L->dlt, &L->ndlt, 8,
                   g, in, off, is_fptr);
}

static lnkent *opd_get(hld_link *L, hld_gsym *g, isec *in, uint64_t off)
{
    return lnk_get(L, L->opd_hash, &L->opd_tail, &L->opd, &L->nopd, 16,
                   g, in, off, 0);
}

static lnkent *pltoff_get(hld_link *L, hld_gsym *g, isec *in, uint64_t off)
{
    return lnk_get(L, L->pltoff_hash, &L->pltoff_tail, &L->pltoff,
                   &L->npltoff, 16, g, in, off, 0);
}

/* The descriptor allocated for a target, if any. */
static lnkent *opd_find(hld_link *L, hld_gsym *g, isec *in, uint64_t off)
{
    lnkent *l;
    for (l = L->opd_hash[lnk_hash(g, in, off)]; l; l = l->hnext)
        if (l->g == g && l->in == in && l->off == off)
            return l;
    return NULL;
}

/*
 * Walk every relocation and reserve the linkage-table slots it will need,
 * before addresses are assigned so the tables can be laid out like any other
 * section.
 *
 *   LTOFF*      -> a DLT slot holding the target's address
 *   LTOFF_FPTR* -> a descriptor, plus a DLT slot holding the descriptor's
 *                  address
 *   FPTR*       -> a descriptor, addressed directly
 */
int hld_alloc_linkage(hld_link *L)
{
    size_t i;
    uint32_t j;

    for (i = 0; i < L->nobjs; i++) {
        hld_elf *e = L->objs[i];
        for (j = 1; j < e->eh.shnum; j++) {
            hld_shdr *rsh = &e->shdrs[j];
            hld_rela *rel;
            hld_sym *syms;
            size_t nrel, nsyms, k;
            char err[HLD_ERRSZ];

            if (rsh->type != SHT_RELA) continue;
            if (rsh->info == 0 || rsh->info >= e->eh.shnum) continue;
            if (!(e->shdrs[rsh->info].flags & SHF_ALLOC)) continue;
            if (!isec_of(L, e, rsh->info)) continue;
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
                uint64_t off;
                const char *nm;
                int want_dlt = 0, want_opd = 0, want_pltoff = 0;

                switch (r->type) {
                case R_IA64_LTOFF22:
                case R_IA64_LTOFF22X:
                case R_IA64_LTOFF64I:
                    want_dlt = 1;
                    break;
                case R_IA64_LTOFF_FPTR22:
                case R_IA64_LTOFF_FPTR64I:
                case R_IA64_LTOFF_FPTR32MSB:
                case R_IA64_LTOFF_FPTR32LSB:
                case R_IA64_LTOFF_FPTR64MSB:
                case R_IA64_LTOFF_FPTR64LSB:
                    want_dlt = want_opd = 1;
                    break;
                case R_IA64_FPTR64I:
                case R_IA64_FPTR32MSB:
                case R_IA64_FPTR32LSB:
                case R_IA64_FPTR64MSB:
                case R_IA64_FPTR64LSB:
                    want_opd = 1;
                    break;
                case R_IA64_PLTOFF22:
                case R_IA64_PLTOFF64I:
                case R_IA64_PLTOFF64MSB:
                case R_IA64_PLTOFF64LSB:
                    want_pltoff = 1;
                    break;
                default:
                    continue;
                }
                if (reloc_target(L, e, syms, nsyms, r, &g, &in, &off, &nm) < 0) {
                    free(syms); free(rel);
                    return -1;
                }
                if (want_opd && !opd_get(L, g, in, off)) goto oom;
                if (want_dlt && !dlt_get(L, g, in, off, want_opd)) goto oom;
                /*
                 * An imported function already has a descriptor in .plt for
                 * the loader to fill; only a local target needs one made here.
                 */
                if (want_pltoff && !(g && g->kind == HLD_SYM_IMPORT)
                    && !pltoff_get(L, g, in, off)) goto oom;
                continue;
oom:
                lerr(L, "out of memory", NULL, NULL);
                free(syms); free(rel);
                return -1;
            }
            free(syms);
            free(rel);
        }
    }

    /*
     * The DLT is reached gp-relatively, so it must sit in the short-addressable
     * data region. Descriptors are reached by absolute address, and in an
     * executable they are fully resolved at link time, so they can live in the
     * read-only text segment (which is where HP's linker puts them too).
     */
    if (L->ndlt) {
        L->dltsec = osec_get(L, ".dlt", SHT_PROGBITS,
                             SHF_ALLOC | SHF_WRITE | SHF_IA_64_SHORT);
        if (!L->dltsec) { lerr(L, "out of memory", NULL, NULL); return -1; }
        L->dltsec->size = L->ndlt * 8;
        L->dltsec->align = 16;
        L->dltsec->entsize = 8;
    }
    if (L->npltoff) {
        /* gp-relative, so it belongs in the short data region */
        L->pltoffsec = osec_get(L, ".pltoff", SHT_PROGBITS,
                                SHF_ALLOC | SHF_WRITE | SHF_IA_64_SHORT);
        if (!L->pltoffsec) { lerr(L, "out of memory", NULL, NULL); return -1; }
        L->pltoffsec->size = L->npltoff * 16;
        L->pltoffsec->align = 16;
        L->pltoffsec->entsize = 16;
    }
    if (L->nopd) {
        L->opdsec = osec_get(L, ".opd", SHT_PROGBITS, SHF_ALLOC);
        if (!L->opdsec) { lerr(L, "out of memory", NULL, NULL); return -1; }
        L->opdsec->size = L->nopd * 16;
        L->opdsec->align = 16;
        L->opdsec->entsize = 16;
    }
    return 0;
}

/* ---- section contents -------------------------------------------------- */

/*
 * Materialize each output section: concatenate its input contributions
 * (NOBITS and synthetic COMMON pieces contribute zeroes). Relocations are
 * applied to these buffers, which are then written out verbatim.
 */
int hld_build_contents(hld_link *L)
{
    osec *o;
    isec *in;
    lnkent *l;
    char err[HLD_ERRSZ];

    for (o = L->osecs; o; o = o->next) {
        if (o->type == SHT_NOBITS || o->size == 0) continue;
        o->data = calloc(1, (size_t)o->size);
        if (!o->data) { lerr(L, "out of memory", NULL, NULL); return -1; }
        for (in = o->first; in; in = in->next) {
            const uint8_t *src;
            if (!in->sh || in->sh->type == SHT_NOBITS || in->sh->size == 0)
                continue;
            src = hld_sec_data(in->obj, in->sh, err);
            if (!src) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }
            memcpy(o->data + in->out_off, src, (size_t)in->sh->size);
        }
    }

    /*
     * Fill the linkage tables now that every address is final. A function
     * descriptor is {entry point, gp}; a DLT slot holds either a plain
     * address or, for the FPTR forms, the address of a descriptor.
     */
    if (L->opdsec && L->opdsec->data)
        for (l = L->opd; l; l = l->next) {
            st64(L->opdsec->data + l->slot, target_addr(l->g, l->in, l->off));
            st64(L->opdsec->data + l->slot + 8, L->gp);
        }
    if (L->pltoffsec && L->pltoffsec->data)
        for (l = L->pltoff; l; l = l->next) {
            st64(L->pltoffsec->data + l->slot, target_addr(l->g, l->in, l->off));
            st64(L->pltoffsec->data + l->slot + 8, L->gp);
        }
    if (L->dltsec && L->dltsec->data)
        for (l = L->dlt; l; l = l->next) {
            uint64_t v;
            if (l->is_fptr) {
                lnkent *d = opd_find(L, l->g, l->in, l->off);
                v = d ? L->opdsec->addr + d->slot : 0;
            } else {
                v = target_addr(l->g, l->in, l->off);
            }
            st64(L->dltsec->data + l->slot, v);
        }
    return 0;
}

int hld_relocate(hld_link *L)
{
    size_t i;
    uint32_t j;

    for (i = 0; i < L->nobjs; i++) {
        hld_elf *e = L->objs[i];
        for (j = 1; j < e->eh.shnum; j++) {
            hld_shdr *rsh = &e->shdrs[j];
            hld_shdr *tsh;
            hld_rela *rel;
            hld_sym *syms;
            size_t nrel, nsyms, k;
            isec *in;
            char err[HLD_ERRSZ];
            uint8_t *dst;

            if (rsh->type != SHT_RELA) continue;
            if (rsh->info == 0 || rsh->info >= e->eh.shnum) continue;
            tsh = &e->shdrs[rsh->info];
            if (!(tsh->flags & SHF_ALLOC)) continue;   /* e.g. .rela.debug_* */

            in = isec_of(L, e, rsh->info);
            if (!in || !in->out->data) continue;

            rel = hld_read_relas(e, rsh, &nrel, err);
            if (!rel) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }
            if (rsh->link >= e->eh.shnum) {
                lerr(L, "%s: rela section has no symbol table", e->path, NULL);
                free(rel);
                return -1;
            }
            syms = hld_read_syms(e, &e->shdrs[rsh->link], &nsyms, err);
            if (!syms) {
                snprintf(L->err, HLD_ERRSZ, "%s", err);
                free(rel);
                return -1;
            }

            dst = in->out->data + in->out_off;
            for (k = 0; k < nrel; k++) {
                const hld_rela *r = &rel[k];
                uint64_t S, P, V;
                uint64_t where = in->out->addr + in->out_off + (r->offset & ~3ULL);
                unsigned slot = (unsigned)(r->offset & 3);
                const char *sname = "";
                hld_gsym *tg;
                isec *tin;
                uint64_t toff;
                lnkent *ent;
                hld_patch_status st;

                if (reloc_target(L, e, syms, nsyms, r, &tg, &tin, &toff,
                                 &sname) < 0) {
                    free(syms); free(rel);
                    return -1;
                }
                S = target_addr(tg, tin, toff);
                P = where;

                switch (r->type) {
                case R_IA64_NONE:
                case R_IA64_LDXMOV:
                    continue;

                case R_IA64_IMM14:
                case R_IA64_IMM22:
                case R_IA64_IMM64:
                case R_IA64_DIR32MSB:
                case R_IA64_DIR32LSB:
                case R_IA64_DIR64MSB:
                case R_IA64_DIR64LSB:
                    V = S;
                    break;

                /* DLT (GOT) access: the gp-relative offset of the slot. */
                case R_IA64_LTOFF22:
                case R_IA64_LTOFF22X:
                case R_IA64_LTOFF64I:
                    ent = dlt_get(L, tg, tin, toff, 0);
                    if (!ent) { lerr(L, "out of memory", NULL, NULL); goto rfail; }
                    V = L->dltsec->addr + ent->slot - L->gp;
                    break;

                /* Same, but the slot holds the address of a descriptor. */
                case R_IA64_LTOFF_FPTR22:
                case R_IA64_LTOFF_FPTR64I:
                case R_IA64_LTOFF_FPTR32MSB:
                case R_IA64_LTOFF_FPTR32LSB:
                case R_IA64_LTOFF_FPTR64MSB:
                case R_IA64_LTOFF_FPTR64LSB:
                    ent = dlt_get(L, tg, tin, toff, 1);
                    if (!ent) { lerr(L, "out of memory", NULL, NULL); goto rfail; }
                    V = L->dltsec->addr + ent->slot - L->gp;
                    break;

                /*
                 * @pltoff: the gp-relative offset of the function's
                 * descriptor. An import uses the .plt slot the loader binds;
                 * anything else uses one built at link time.
                 */
                case R_IA64_PLTOFF22:
                case R_IA64_PLTOFF64I:
                case R_IA64_PLTOFF64MSB:
                case R_IA64_PLTOFF64LSB:
                    if (tg && tg->kind == HLD_SYM_IMPORT) {
                        if (!L->pltsec) {
                            snprintf(L->err, HLD_ERRSZ,
                                     "%s: `%s' needs an import descriptor",
                                     e->path, sname);
                            goto rfail;
                        }
                        V = L->pltsec->addr + tg->plt_slot - L->gp;
                    } else {
                        ent = pltoff_get(L, tg, tin, toff);
                        if (!ent) { lerr(L, "out of memory", NULL, NULL); goto rfail; }
                        V = L->pltoffsec->addr + ent->slot - L->gp;
                    }
                    break;

                /*
                 * An import-PLT relocation inside an input object asks for a
                 * function descriptor to be written at the site itself —
                 * C++ vtable slots are built this way. Sixteen bytes, so it
                 * is placed here rather than through the field inserter.
                 */
                case R_IA64_IPLTMSB:
                case R_IA64_IPLTLSB:
                    if (tg && tg->kind == HLD_SYM_IMPORT) {
                        snprintf(L->err, HLD_ERRSZ,
                                 "%s: `%s' is imported and needs a descriptor "
                                 "bound at run time, which hld cannot emit yet",
                                 e->path, sname);
                        goto rfail;
                    }
                    if (r->type == R_IA64_IPLTMSB) {
                        st64(dst + r->offset, S);
                        st64(dst + r->offset + 8, L->gp);
                    } else {
                        stle64(dst + r->offset, S);
                        stle64(dst + r->offset + 8, L->gp);
                    }
                    continue;

                /* The descriptor's own address. */
                case R_IA64_FPTR64I:
                case R_IA64_FPTR32MSB:
                case R_IA64_FPTR32LSB:
                case R_IA64_FPTR64MSB:
                case R_IA64_FPTR64LSB:
                    ent = opd_find(L, tg, tin, toff);
                    if (!ent) {
                        snprintf(L->err, HLD_ERRSZ,
                                 "%s: no descriptor allocated for `%s'",
                                 e->path, sname);
                        goto rfail;
                    }
                    V = L->opdsec->addr + ent->slot;
                    break;

                case R_IA64_GPREL22:
                case R_IA64_GPREL64I:
                case R_IA64_GPREL32MSB:
                case R_IA64_GPREL32LSB:
                case R_IA64_GPREL64MSB:
                case R_IA64_GPREL64LSB:
                    V = S - L->gp;
                    break;

                case R_IA64_PCREL21B:
                case R_IA64_PCREL21BI:
                case R_IA64_PCREL21F:
                case R_IA64_PCREL21M:
                case R_IA64_PCREL22:
                case R_IA64_PCREL60B:
                case R_IA64_PCREL64I:
                case R_IA64_PCREL32MSB:
                case R_IA64_PCREL32LSB:
                case R_IA64_PCREL64MSB:
                case R_IA64_PCREL64LSB:
                    V = S - P;
                    break;

                case R_IA64_SEGREL32MSB:
                case R_IA64_SEGREL32LSB:
                case R_IA64_SEGREL64MSB:
                case R_IA64_SEGREL64LSB: {
                    uint64_t base = (S >= HLD_DATA_BASE) ? L->data_addr
                                                         : L->text_addr;
                    V = S - base;
                    break;
                }

                case R_IA64_SECREL32MSB:
                case R_IA64_SECREL32LSB:
                case R_IA64_SECREL64MSB:
                case R_IA64_SECREL64LSB:
                    V = S - in->out->addr;
                    break;

                default:
                    snprintf(L->err, HLD_ERRSZ,
                             "%s: relocation %s against `%s' is not implemented yet "
                             "(needs DLT/function-descriptor support)",
                             e->path,
                             hld_reloc_name(r->type) ? hld_reloc_name(r->type)
                                                     : "of unknown type",
                             sname);
                    free(syms); free(rel);
                    return -1;
                }

                st = hld_ia64_install_value(dst + (r->offset & ~3ULL), slot, V,
                                            r->type);
                if (st == HLD_PATCH_OVERFLOW) {
                    snprintf(L->err, HLD_ERRSZ,
                             "%s: relocation %s against `%s' overflows "
                             "(value 0x%llx at 0x%llx)",
                             e->path,
                             hld_reloc_name(r->type) ? hld_reloc_name(r->type) : "?",
                             sname, U(V), U(where));
                    free(syms); free(rel);
                    return -1;
                }
                if (st != HLD_PATCH_OK) {
                    snprintf(L->err, HLD_ERRSZ,
                             "%s: cannot apply relocation %s against `%s'",
                             e->path,
                             hld_reloc_name(r->type) ? hld_reloc_name(r->type) : "?",
                             sname);
                    goto rfail;
                }
                continue;
rfail:
                free(syms); free(rel);
                return -1;
            }
            free(syms);
            free(rel);
        }
    }
    return 0;
}

/* ---- map --------------------------------------------------------------- */

void hld_print_map(hld_link *L)
{
    osec *o;
    unsigned h;
    hld_gsym *g;

    printf("Entry symbol  : %s = 0x%llx\n", L->entry_name, U(L->entry));
    printf("\nSegment -- loadable executable\n\n");
    for (o = L->osecs; o; o = o->next)
        if (sec_is_text(o->flags) && o->type != SHT_NOBITS)
            printf("    %-24s 0x%016llx  size 0x%llx\n", o->name, U(o->addr),
                   U(o->size));
    printf("\nSegment -- loadable writable\n\n");
    for (o = L->osecs; o; o = o->next)
        if (!sec_is_text(o->flags) && (o->flags & SHF_ALLOC))
            printf("    %-24s 0x%016llx  size 0x%llx%s\n", o->name, U(o->addr),
                   U(o->size), o->type == SHT_NOBITS ? "  (nobits)" : "");
    printf("\n    %-24s 0x%016llx\n", "__gp", U(L->gp));
    printf("\nGlobal symbols\n\n");
    for (h = 0; h < HLD_SYMHASH; h++)
        for (g = L->hash[h]; g; g = g->next)
            if (g->kind == HLD_SYM_DEFINED || g->kind == HLD_SYM_ABS)
                printf("    %-32s 0x%016llx\n", g->name, U(g->value));
}

void hld_link_free(hld_link *L)
{
    size_t i;
    osec *o, *on;
    unsigned h;

    for (i = 0; i < L->nobjs; i++) hld_elf_free(L->objs[i]);
    free(L->objs);
    {
        hld_archive *ar, *arn;
        for (ar = L->archives; ar; ar = arn) { arn = ar->next; hld_archive_free(ar); }
    }
    for (o = L->osecs; o; o = on) {
        isec *in, *inn;
        on = o->next;
        for (in = o->first; in; in = inn) { inn = in->next; free(in); }
        free(o->data);
        free(o);
    }
    for (h = 0; h < HLD_SYMHASH; h++) {
        hld_gsym *g, *gn;
        for (g = L->hash[h]; g; g = gn) {
            gn = g->next;
            free((void *)g->name);
            free(g);
        }
    }
}
