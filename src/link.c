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

/*
 * Stamp the output with which linker built it. Not loaded — it costs the
 * running program nothing — but present in the file, in the platform's own
 * `what` format, so `what <binary>` answers the question directly. Which
 * linker produced a binary is the first thing anyone asks when one is
 * suspected of building it wrong, and timestamps are a poor substitute.
 */
int hld_add_ident(hld_link *L)
{
    osec *o = osec_get(L, ".comment", SHT_PROGBITS, 0);

    if (!o) { lerr(L, "out of memory", NULL, NULL); return -1; }
    o->align = 1;
    o->size = strlen(HLD_IDENT) + 1;
    L->commentsec = o;
    return 0;
}

/* ---- output sections --------------------------------------------------- */

/*
 * Name hash for the output-section table. The names that matter here share a
 * long common prefix (`.gnu.linkonce.t._ZN...'), so hash the whole string
 * rather than a prefix of it.
 */
static unsigned osec_hashval(const char *name)
{
    unsigned h = 0;
    const unsigned char *p = (const unsigned char *)name;

    while (*p) h = h * 31u + *p++;
    return h % HLD_OSECHASH;
}

static osec *osec_find(hld_link *L, const char *name)
{
    osec *o;
    for (o = L->osec_hash[osec_hashval(name)]; o; o = o->hnext)
        if (strcmp(o->name, name) == 0)
            return o;
    return NULL;
}

osec *osec_find_pub(hld_link *L, const char *name)
{
    return osec_find(L, name);
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
    {   /* and into the name index the lookup above uses */
        unsigned hv = osec_hashval(name);
        o->hnext = L->osec_hash[hv];
        L->osec_hash[hv] = o;
    }
    L->nosecs++;
    return o;
}

/*
 * Output order. Within a segment, sections keep the order they are first
 * seen except that NOBITS is forced last (it has no file image). The text
 * segment holds alloc sections without SHF_WRITE, the data segment the rest.
 */
/*
 * Is this address inside code the image actually maps? A branch is only
 * meaningful if it is. Checking the encoded displacement is not enough: a
 * wrong target that happens to land within reach encodes perfectly well and
 * faults only when the call is taken, which may be days of work later.
 */
/*
 * Built once, the first time a branch is checked. Walking the section list per
 * relocation is quadratic, and a C++ link has tens of thousands of sections
 * (one `.gnu.linkonce.t.*' per template instantiation) against hundreds of
 * thousands of branches -- which turned a 20-second link into ten minutes.
 */
static int coderange_cmp(const void *a, const void *b)
{
    uint64_t x = ((const coderange *)a)->lo, y = ((const coderange *)b)->lo;
    return x < y ? -1 : x > y ? 1 : 0;
}

static int addr_is_code(hld_link *L, uint64_t a)
{
    size_t lo, hi;

    if (!L->coderanges) {
        osec *o;
        size_t n = 0;
        for (o = L->osecs; o; o = o->next)
            if ((o->flags & SHF_ALLOC) && (o->flags & SHF_EXECINSTR)
                && o->type != SHT_NOBITS && o->size)
                n++;
        L->coderanges = malloc((n ? n : 1) * sizeof *L->coderanges);
        if (!L->coderanges) return 1;      /* cannot check; do not reject */
        L->ncoderanges = 0;
        for (o = L->osecs; o; o = o->next)
            if ((o->flags & SHF_ALLOC) && (o->flags & SHF_EXECINSTR)
                && o->type != SHT_NOBITS && o->size) {
                L->coderanges[L->ncoderanges].lo = o->addr;
                L->coderanges[L->ncoderanges].hi = o->addr + o->size;
                L->ncoderanges++;
            }
        qsort(L->coderanges, L->ncoderanges, sizeof *L->coderanges,
              coderange_cmp);
    }

    /* Last range whose lo <= a, then one containment test. */
    lo = 0; hi = L->ncoderanges;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (L->coderanges[mid].lo <= a) lo = mid + 1; else hi = mid;
    }
    return lo > 0 && a < L->coderanges[lo - 1].hi;
}

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
            const char *oname;

            /*
             * Debug information is not loaded, but it does have to be
             * carried through and relocated, or the output cannot be
             * debugged at all — and silently so, since nothing about the
             * link fails. Other non-allocated sections (.comment, the
             * relocation and symbol tables) are the linker's own business
             * and are not copied.
             */
            if (!(sh->flags & SHF_ALLOC)
                && !(sh->type == SHT_PROGBITS
                     && strncmp(sh->name, ".debug", 6) == 0))
                continue;
            if (sh->type == SHT_GROUP)
                continue;

            /*
             * The compiler can emit a separate unwind section per function
             * (.IA_64.unwind.text.NAME and its descriptors). They all have to
             * end up in one table, contiguous and sorted, or the runtime
             * cannot find the entry for a function it is unwinding through.
             */
            oname = sh->name;
            if (sh->type == SHT_IA_64_UNWIND)
                oname = ".IA_64.unwind";
            else if (strncmp(sh->name, ".IA_64.unwind_info", 18) == 0)
                oname = ".IA_64.unwind_info";

            o = osec_get(L, oname, sh->type, sh->flags);
            if (!o) { lerr(L, "out of memory", NULL, NULL); return -1; }
            if (sh->addralign > o->align) o->align = sh->addralign;
            if (sh->entsize && !o->entsize) o->entsize = sh->entsize;

            in = calloc(1, sizeof *in);
            if (!in) { lerr(L, "out of memory", NULL, NULL); return -1; }
            in->obj = e;
            in->idx = j;
            in->sh = sh;
            in->out = o;
            if (!e->isec_by_shndx) {
                e->isec_by_shndx = calloc(e->eh.shnum, sizeof *e->isec_by_shndx);
                if (!e->isec_by_shndx) {
                    lerr(L, "out of memory", NULL, NULL);
                    free(in);
                    return -1;
                }
            }
            e->isec_by_shndx[j] = in;
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
isec *hld_isec_of(hld_link *L, hld_elf *obj, uint32_t shndx)
{
    (void)L;
    if (!obj->isec_by_shndx || shndx >= obj->eh.shnum) return NULL;
    return (isec *)obj->isec_by_shndx[shndx];
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
                    isec *in = hld_isec_of(L, e, s->shndx);
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

/*
 * The unwind table needs a header describing it, which no input provides:
 * three doublewords holding a version and the segment-relative bounds of the
 * table. It is reserved here and filled once addresses are final.
 */
#define UNWIND_HDR_SIZE 24

int hld_alloc_unwind(hld_link *L)
{
    osec *o, *unw = NULL;

    for (o = L->osecs; o; o = o->next)
        if (o->type == SHT_IA_64_UNWIND && o->size) { unw = o; break; }
    if (!unw) return 0;

    L->unwind_sec = unw;
    L->unwind_info_sec = osec_find(L, ".IA_64.unwind_info");
    o = osec_get(L, ".IA_64.unwind_hdr", SHT_PROGBITS, SHF_ALLOC);
    if (!o) { lerr(L, "out of memory", NULL, NULL); return -1; }
    o->size = UNWIND_HDR_SIZE;
    o->align = 8;
    L->unwind_hdr_sec = o;
    return 0;
}

/*
 * Entries have to be sorted by the address they describe: the runtime
 * searches the table rather than walking it. Sorting happens after
 * relocation, when the segment-relative values in each entry are final.
 */
/* Ordered by the address each entry describes; see hld_finish_unwind(). */
static int unwind_entry_cmp(const void *a, const void *b)
{
    uint64_t x = be64((const uint8_t *)a), y = be64((const uint8_t *)b);
    return x < y ? -1 : x > y ? 1 : 0;
}

int hld_finish_unwind(hld_link *L)
{
    uint8_t *p;
    size_t n;

    if (!L->unwind_sec || !L->unwind_sec->data) return 0;

    p = L->unwind_sec->data;
    n = (size_t)(L->unwind_sec->size / UNWIND_HDR_SIZE);
    /*
     * This was an insertion sort, on the reasoning that the table arrives
     * nearly ordered. That holds only while the output section order matches
     * the order entries were collected in -- and C++ COMDAT breaks it: one
     * `.gnu.linkonce.t.*' per template instantiation interleaves tens of
     * thousands of sections, leaving the table far from sorted. Measured at
     * 43,491 entries it cost 63.8s of the link; qsort makes it negligible.
     */
    qsort(p, n, UNWIND_HDR_SIZE, unwind_entry_cmp);

    if (L->unwind_hdr_sec && L->unwind_hdr_sec->data) {
        uint8_t *h = L->unwind_hdr_sec->data;
        st64(h + 0, 2);                          /* version, as HP ld writes */
        st64(h + 8, L->unwind_sec->addr - L->text_addr);
        st64(h + 16, L->unwind_sec->addr + L->unwind_sec->size - L->text_addr);
    }
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
    "__unwind_header",
    NULL
};

/*
 * Is this a name the linker defines for itself? Such a symbol describes this
 * module's own layout, so it must never be resolved to a shared library that
 * exports the same name -- the program would read another module's addresses.
 */
int hld_is_linker_symbol(const char *name)
{
    const char *const *n;

    for (n = hld_linker_symbols; *n; n++)
        if (strcmp(*n, name) == 0) return 1;
    return 0;
}

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
    uint32_t nphdr;

    /*
     * Thread-local sections are a template, copied per thread, so they lead
     * the data segment and the ordinary sections that follow reuse the
     * addresses the .tbss part occupies. The first 16 bytes of the template
     * are reserved — the platform's linker leaves them, and an access
     * compiled as an offset from the thread pointer expects them to be there.
     */
    for (o = L->osecs; o; o = o->next)
        if ((o->flags & SHF_ALLOC) && (o->flags & SHF_TLS) && o->size)
            L->has_tls = 1;

    nphdr = (L->dynamic ? 5 : 3) + (L->has_tls ? 1 : 0)
            + (L->unwind_sec ? 1 : 0);

    L->nphdr = nphdr;

    /* text segment */
    off = (uint64_t)EHDR64_SIZE + (uint64_t)nphdr * PHDR64_SIZE;
    addr = HLD_TEXT_BASE + off;
    L->text_addr = HLD_TEXT_BASE;

    /*
     * The unwind header, table and descriptors lead the text segment and stay
     * adjacent, so one program header can describe the lot — the arrangement
     * the platform's linker produces.
     */
    {
        osec *ord[3];
        int k;
        ord[0] = L->unwind_hdr_sec;
        ord[1] = L->unwind_sec;
        ord[2] = L->unwind_info_sec;
        for (k = 0; k < 3; k++) {
            o = ord[k];
            if (!o || !sec_is_text(o->flags) || o->type == SHT_NOBITS) continue;
            addr = align_up(addr, o->align);
            off = align_up(off, o->align);
            o->addr = addr; o->off = off;
            addr += o->size; off += o->size;
        }
    }

    for (o = L->osecs; o; o = o->next) {
        if (!sec_is_text(o->flags)) continue;
        if (o->type == SHT_NOBITS) continue;    /* no NOBITS in text */
        if (o == L->unwind_hdr_sec || o == L->unwind_sec
            || o == L->unwind_info_sec) continue;
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

#define IS_DATA(o) (!sec_is_text((o)->flags) && ((o)->flags & SHF_ALLOC) \
                    && !((o)->flags & SHF_TLS))
#define IS_SHORT(o) (((o)->flags & SHF_IA_64_SHORT) != 0)

    if (L->has_tls) {
        uint64_t tls_end;

        L->tls_base = addr;
        L->tls_off = off;
        addr += 16;
        off += 16;

        for (o = L->osecs; o; o = o->next) {       /* .tdata */
            if (!(o->flags & SHF_TLS) || o->type == SHT_NOBITS) continue;
            addr = align_up(addr, o->align);
            off = align_up(off, o->align);
            o->addr = addr; o->off = off;
            addr += o->size; off += o->size;
        }
        L->tls_filesz = addr - L->tls_base;

        tls_end = addr;
        for (o = L->osecs; o; o = o->next) {       /* .tbss */
            if (!(o->flags & SHF_TLS) || o->type != SHT_NOBITS) continue;
            tls_end = align_up(tls_end, o->align);
            o->addr = tls_end; o->off = off;
            tls_end += o->size;
        }
        L->tls_memsz = align_up(tls_end - L->tls_base, 16);
    }

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

    /*
     * A shared library anchors gp inside the region reserved for the loader,
     * the way the platform's linker does, rather than on the first word of
     * its own short data. The loader writes its own words at gp; with gp
     * pointing at a variable instead, that variable is quietly overwritten
     * when the library is loaded, and the program reads whatever the loader
     * left behind.
     */
    if (L->shared && L->reservesec)
        L->gp = L->reservesec->addr + L->reserve_off + 8;

    /*
     * Sections that are not part of the image follow it in the file, with no
     * address of their own. Their relocations then resolve section-relative,
     * which is exactly what the debug format's references between sections
     * are expressed as.
     */
    off = L->data_off + L->data_filesz;
    for (o = L->osecs; o; o = o->next) {
        if ((o->flags & SHF_ALLOC) || o->type == SHT_NOBITS || !o->size)
            continue;
        off = align_up(off, o->align);
        o->addr = 0;
        o->off = off;
        off += o->size;
    }

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
    if (def_abs(L, "__unwind_header",
                L->unwind_hdr_sec ? L->unwind_hdr_sec->addr : 0) < 0) return -1;
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
    if (def_abs(L, "__TLS_SIZE", L->tls_memsz) < 0) return -1;
    if (def_abs(L, "__TLS_INIT_SIZE", L->tls_filesz) < 0) return -1;
    if (def_abs(L, "__TLS_INIT_START", L->tls_base) < 0) return -1;
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
    if (!L->entry_name) { L->entry = 0; return 0; }   /* a library has none */
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
int hld_reloc_target(hld_link *L, hld_elf *e, hld_sym *syms, size_t nsyms,
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
    in = hld_isec_of(L, e, s->shndx);
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
uint64_t hld_target_addr(hld_gsym *g, isec *in, uint64_t off)
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
                       hld_gsym *g, isec *in, uint64_t off, int kind)
{
    unsigned h = lnk_hash(g, in, off);
    lnkent *l;

    (void)L;
    for (l = hash[h]; l; l = l->hnext)
        if (l->g == g && l->in == in && l->off == off && l->kind == kind)
            return l;
    l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->g = g;
    l->in = in;
    l->off = off;
    l->kind = kind;
    l->slot = *count * entsize;
    (*count)++;
    l->hnext = hash[h];
    hash[h] = l;
    if (!*head) { *head = l; *tail = &l->next; }
    else { **tail = l; *tail = &l->next; }
    return l;
}

static lnkent *dlt_get(hld_link *L, hld_gsym *g, isec *in, uint64_t off,
                       int kind)
{
    return lnk_get(L, L->dlt_hash, &L->dlt_tail, &L->dlt, &L->ndlt, 8,
                   g, in, off, kind);
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
 * True when a relocation stores the address of an imported symbol into a
 * data word. hld has no address to store: the symbol belongs to a shared
 * library, so the loader must write the word from a dynamic relocation.
 * Everything else about an import resolves here — a call goes to the stub,
 * and a linkage-table slot is handled with the rest of the table.
 */
static int hld_dynrel_type(const hld_gsym *g, uint32_t type, uint32_t *out)
{
    if (!g || g->kind != HLD_SYM_IMPORT) return 0;
    switch (type) {
    case R_IA64_DIR64MSB:
    case R_IA64_DIR64LSB:
        *out = R_IA64_DIR64MSB;
        return 1;
    case R_IA64_FPTR64MSB:
    case R_IA64_FPTR64LSB:
        /* The canonical descriptor is the defining module's to make. */
        *out = R_IA64_FPTR64MSB;
        return 1;
    default:
        return 0;
    }
}

static int dynrel_add(hld_link *L, isec *in, uint64_t off, hld_gsym *g,
                      uint64_t addend, uint32_t type)
{
    if (L->ndynrel == L->dynrel_cap) {
        size_t cap = L->dynrel_cap ? L->dynrel_cap * 2 : 16;
        dynrel *n = realloc(L->dynrels, cap * sizeof *n);
        if (!n) return -1;
        L->dynrels = n;
        L->dynrel_cap = cap;
    }
    L->dynrels[L->ndynrel].in = in;
    L->dynrels[L->ndynrel].off = off;
    L->dynrels[L->ndynrel].g = g;
    L->dynrels[L->ndynrel].addend = addend;
    L->dynrels[L->ndynrel].type = type;
    L->ndynrel++;
    return 0;
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
            isec *site;

            if (rsh->type != SHT_RELA) continue;
            if (rsh->info == 0 || rsh->info >= e->eh.shnum) continue;
            if (!(e->shdrs[rsh->info].flags & SHF_ALLOC)) continue;
            site = hld_isec_of(L, e, rsh->info);
            if (!site) continue;
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
                int want_dlt = 0, want_opd = 0, want_pltoff = 0, want_dyn = 0;
                int dlt_kind = HLD_DLT_PLAIN;
                uint32_t dtype;

                switch (r->type) {
                case R_IA64_LTOFF22:
                case R_IA64_LTOFF22X:
                case R_IA64_LTOFF64I:
                    want_dlt = 1;
                    break;
                /*
                 * Thread-local, reached through the table: the slot holds the
                 * variable's offset from the thread pointer rather than its
                 * address. In an executable that offset is settled here, so
                 * no help from the loader is needed.
                 */
                case R_IA64_LTOFF_TPREL22:
                    want_dlt = 1;
                    dlt_kind = HLD_DLT_TPREL;
                    break;
                /*
                 * General dynamic: two slots, module and offset, which the
                 * code hands to __tls_get_addr. In a program both are known
                 * here — it is its own module and nothing can interpose.
                 */
                case R_IA64_LTOFF_DTPMOD22:
                    want_dlt = 1;
                    dlt_kind = HLD_DLT_DTPMOD;
                    break;
                case R_IA64_LTOFF_DTPREL22:
                    want_dlt = 1;
                    dlt_kind = HLD_DLT_DTPREL;
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
                    want_opd = 1;
                    break;
                case R_IA64_FPTR64MSB:
                case R_IA64_FPTR64LSB:
                    want_opd = want_dyn = 1;
                    break;
                case R_IA64_DIR64MSB:
                case R_IA64_DIR64LSB:
                    want_dyn = 1;
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
                if (hld_reloc_target(L, e, syms, nsyms, r, &g, &in, &off, &nm) < 0) {
                    free(syms); free(rel);
                    return -1;
                }
                if (want_dyn && hld_dynrel_type(g, r->type, &dtype)) {
                    if (dynrel_add(L, site, r->offset, g, off, dtype) < 0) goto oom;
                    /* The other module owns the descriptor; don't make one. */
                    continue;
                }
                if (want_opd) dlt_kind = HLD_DLT_FPTR;
                if ((dlt_kind == HLD_DLT_TPREL || dlt_kind == HLD_DLT_DTPMOD
                     || dlt_kind == HLD_DLT_DTPREL) && g
                    && g->kind == HLD_SYM_IMPORT) {
                    lerr(L, "thread-local `%s' is defined in a shared library;"
                            " hld cannot resolve that yet", nm, NULL);
                    free(syms); free(rel);
                    return -1;
                }
                if (want_opd && !opd_get(L, g, in, off)) goto oom;
                if (want_dlt && !dlt_get(L, g, in, off, dlt_kind)) goto oom;
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
    if (L->nopd && L->shared && L->opdsec)
        L->opdsec->flags |= SHF_WRITE;   /* a library's descriptors are data */
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

    if (L->commentsec && L->commentsec->data)
        memcpy(L->commentsec->data, HLD_IDENT, strlen(HLD_IDENT) + 1);

    /*
     * Fill the linkage tables now that every address is final. A function
     * descriptor is {entry point, gp}; a DLT slot holds either a plain
     * address or, for the FPTR forms, the address of a descriptor.
     */
    if (L->opdsec && L->opdsec->data)
        for (l = L->opd; l; l = l->next) {
            st64(L->opdsec->data + l->slot, hld_target_addr(l->g, l->in, l->off));
            st64(L->opdsec->data + l->slot + 8, L->gp);
        }
    if (L->pltoffsec && L->pltoffsec->data)
        for (l = L->pltoff; l; l = l->next) {
            st64(L->pltoffsec->data + l->slot, hld_target_addr(l->g, l->in, l->off));
            st64(L->pltoffsec->data + l->slot + 8, L->gp);
        }
    if (L->dltsec && L->dltsec->data)
        for (l = L->dlt; l; l = l->next) {
            uint64_t v;
            if (l->g && l->g->kind == HLD_SYM_IMPORT) {
                /*
                 * The target lives in a shared library, so its address is
                 * not ours to write: the loader fills this slot from the
                 * dynamic relocation emitted alongside it. A plain address
                 * is seeded with the link-time binding as a hint, the way
                 * the platform's linker does; a descriptor slot stays zero,
                 * because only the loader can make the canonical descriptor
                 * for a function it owns.
                 */
                v = l->kind == HLD_DLT_FPTR ? 0 : l->g->hint;
            } else if (l->kind == HLD_DLT_FPTR) {
                lnkent *d = opd_find(L, l->g, l->in, l->off);
                v = d ? L->opdsec->addr + d->slot : 0;
            } else if (l->kind == HLD_DLT_TPREL) {
                v = hld_target_addr(l->g, l->in, l->off) - L->tls_base;
            } else if (l->kind == HLD_DLT_DTPMOD) {
                v = ~(uint64_t)0;          /* this module, as HP ld writes it */
            } else if (l->kind == HLD_DLT_DTPREL) {
                v = hld_target_addr(l->g, l->in, l->off) - L->tls_base;
            } else {
                v = hld_target_addr(l->g, l->in, l->off);
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
            hld_rela *rel;
            hld_sym *syms;
            size_t nrel, nsyms, k;
            isec *in;
            char err[HLD_ERRSZ];
            uint8_t *dst;

            if (rsh->type != SHT_RELA) continue;
            if (rsh->info == 0 || rsh->info >= e->eh.shnum) continue;
            /*
             * Anything collected gets relocated, whether it is loaded or
             * not; a section that was not collected has no record here and
             * is skipped below.
             */
            in = hld_isec_of(L, e, rsh->info);
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
                /*
                 * Only an instruction relocation carries the bundle slot in
                 * r_offset's low bits; a whole-word store uses the offset as
                 * it stands, which in debug information is often unaligned.
                 */
                int insn = hld_ia64_reloc_is_insn(r->type);
                uint64_t at = insn ? (r->offset & ~3ULL) : r->offset;
                uint64_t where = in->out->addr + in->out_off + at;
                unsigned slot = insn ? (unsigned)(r->offset & 3) : 0;
                const char *sname = "";
                hld_gsym *tg;
                isec *tin;
                uint64_t toff;
                lnkent *ent;
                hld_patch_status st;
                uint32_t dyntype;

                if (hld_reloc_target(L, e, syms, nsyms, r, &tg, &tin, &toff,
                                 &sname) < 0) {
                    free(syms); free(rel);
                    return -1;
                }
                S = hld_target_addr(tg, tin, toff);
                P = where;

                /*
                 * An import's `value` is its call stub, which is the right
                 * answer for a branch and meaningless for a data word. Seed
                 * the word with the link-time binding instead — the loader
                 * overwrites it from the dynamic relocation recorded for this
                 * site when the linkage tables were built.
                 */
                if (hld_dynrel_type(tg, r->type, &dyntype))
                    S = tg->hint + toff;

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
                case R_IA64_LTOFF_TPREL22:
                    ent = dlt_get(L, tg, tin, toff, HLD_DLT_TPREL);
                    if (!ent) { lerr(L, "out of memory", NULL, NULL); goto rfail; }
                    V = L->dltsec->addr + ent->slot - L->gp;
                    break;

                case R_IA64_LTOFF_DTPMOD22:
                    ent = dlt_get(L, tg, tin, toff, HLD_DLT_DTPMOD);
                    if (!ent) { lerr(L, "out of memory", NULL, NULL); goto rfail; }
                    V = L->dltsec->addr + ent->slot - L->gp;
                    break;

                case R_IA64_LTOFF_DTPREL22:
                    ent = dlt_get(L, tg, tin, toff, HLD_DLT_DTPREL);
                    if (!ent) { lerr(L, "out of memory", NULL, NULL); goto rfail; }
                    V = L->dltsec->addr + ent->slot - L->gp;
                    break;

                /* The same two quantities, written straight into data. */
                case R_IA64_DTPMOD64MSB:
                case R_IA64_DTPMOD64LSB:
                    V = ~(uint64_t)0;
                    break;

                case R_IA64_DTPREL14:
                case R_IA64_DTPREL22:
                case R_IA64_DTPREL64I:
                case R_IA64_DTPREL32MSB:
                case R_IA64_DTPREL32LSB:
                case R_IA64_DTPREL64MSB:
                case R_IA64_DTPREL64LSB:
                    V = S - L->tls_base;
                    break;

                case R_IA64_LTOFF22:
                case R_IA64_LTOFF22X:
                case R_IA64_LTOFF64I:
                    ent = dlt_get(L, tg, tin, toff, HLD_DLT_PLAIN);
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
                    ent = dlt_get(L, tg, tin, toff, HLD_DLT_FPTR);
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
                    /*
                     * S - gp is only load-invariant while the target and gp
                     * move together. In a shared library they need not: the
                     * loader maps the segments independently and does not
                     * preserve the distance between them. Measured on the
                     * target -- one library's .rodata and .data were
                     * 0x2000000000000000 apart at link time and
                     * 0xdfffffffea79ddb8 apart once loaded.
                     *
                     * So a gp-relative value reaching outside the segment gp
                     * lives in cannot be written by any linker, and no
                     * dynamic relocation rescues it: the loader fixes data
                     * words, not the immediate inside an instruction. The
                     * compiler has to use a linkage-table slot instead
                     * (LTOFF22X), which gcc 4.7.4 does and gcc 9.5 does not.
                     *
                     * Refuse rather than emit an address that looks plausible
                     * and faults when it is used.
                     */
                    if (L->shared && S < L->data_addr) {
                        const char *rn = hld_reloc_name(r->type);

                        snprintf(L->err, HLD_ERRSZ,
                                 "%s: %s against `%s' reaches outside the "
                                 "data segment; gp-relative addressing cannot "
                                 "be used that way in a shared library. The "
                                 "address must come from the linkage table "
                                 "instead (@ltoff, not @gprel64)",
                                 e->path, rn ? rn : "a gp-relative relocation",
                                 sname ? sname : "a local symbol");
                        goto rfail;
                    }
                    V = S - L->gp;
                    break;

                case R_IA64_PCREL21B:
                    /*
                     * A direct call reaches +-16 MB. Past that the call is
                     * sent to a stub placed near it, which makes the jump
                     * with a wide branch; the stub leaves b0 alone, so the
                     * target still returns to this caller.
                     */
                    if (!hld_branch_in_range(P, S)) {
                        stubent *sb = hld_stub_find(L, P, tg, tin, toff);
                        if (!sb) {
                            lerr(L, "call to %s is out of reach and has no "
                                    "long-branch stub", sname, NULL);
                            goto rfail;
                        }
                        S = hld_stub_addr(L, sb);
                    }
                    /*
                     * Whatever it resolved to, a call has to land in code.
                     * A symbol that turns out to name data, or a stub whose
                     * address came out wrong, both produce a branch that
                     * encodes cleanly and dies when taken.
                     */
                    if (!addr_is_code(L, S)) {
                        snprintf(L->err, HLD_ERRSZ,
                                 "%s: call to `%s' resolves to 0x%llx, which is "
                                 "not in any code section (call site 0x%llx)",
                                 e->path, sname[0] ? sname : "a local target",
                                 U(S), U(P));
                        goto rfail;
                    }
                    V = S - P;
                    break;

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

                case R_IA64_TPREL14:
                case R_IA64_TPREL22:
                case R_IA64_TPREL64I:
                case R_IA64_TPREL64MSB:
                case R_IA64_TPREL64LSB:
                    if (!L->has_tls) {
                        snprintf(L->err, HLD_ERRSZ,
                                 "%s: `%s' is thread-local but no thread-local "
                                 "storage was laid out", e->path, sname);
                        goto rfail;
                    }
                    V = S - L->tls_base;
                    break;

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

                st = hld_ia64_install_value(dst + at, slot, V, r->type);
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

    printf("Entry symbol  : %s = 0x%llx\n",
           L->entry_name ? L->entry_name : "(none, shared library)", U(L->entry));
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
    free(L->dynrels);
    free(L->coderanges);
    free(L->libpaths);
    free(L->rpaths);
    hld_free_stubs(L);
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
