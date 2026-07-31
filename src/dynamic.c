/*
 * dynamic.c — dynamic-executable output: the sections and tags the HP-UX
 * dynamic loader needs in order to accept and start an image.
 *
 * Which tags are actually mandatory was determined by removing them one at a
 * time from a working binary and running the result (see
 * docs/format-notes.md). For an image with no imports the loader insists on
 * DT_HASH and DT_HP_LOAD_MAP; once there are imports it additionally needs
 * DT_STRTAB, DT_SYMTAB, DT_PLTGOT, DT_RELA, DT_JMPREL, DT_IA_64_PLT_RESERVE
 * and DT_HP_DLD_FLAGS. hld emits the full set regardless — matching what the
 * platform's own linker produces is the safe policy.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "ia64_patch.h"

#define U(x) ((unsigned long long)(x))

/* The loader is named as a colon-separated search list. */
static const char hld_interp[] = "/usr/lib/hpux64/uld.so:/usr/lib/hpux64/dld.so";

/* Reserved for the loader's own use, named by DT_IA_64_PLT_RESERVE. */
#define PLT_RESERVE_SIZE 24
/* One word the loader fills with the address of its load map. */
#define LOAD_MAP_SIZE     8

/* Standard ELF symbol hash (the gABI function). */
static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0, g;
    const unsigned char *p = (const unsigned char *)name;

    while (*p) {
        h = (h << 4) + *p++;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* Append to a growable string table; returns the offset. */
static uint32_t dynstr_add(hld_link *L, const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t off;

    if (L->dynstr_len + n > L->dynstr_cap) {
        size_t nc = L->dynstr_cap ? L->dynstr_cap : 64;
        char *nb;
        while (nc < L->dynstr_len + n) nc *= 2;
        nb = realloc(L->dynstr, nc);
        if (!nb) return 0;
        memset(nb + L->dynstr_cap, 0, nc - L->dynstr_cap);
        L->dynstr = nb;
        L->dynstr_cap = nc;
    }
    if (L->dynstr_len == 0) {          /* index 0 is the empty string */
        L->dynstr[0] = 0;
        L->dynstr_len = 1;
    }
    off = (uint32_t)L->dynstr_len;
    memcpy(L->dynstr + L->dynstr_len, s, n);
    L->dynstr_len += n;
    return off;
}

/*
 * Import stub, two bundles. Assembled from this source and captured here so
 * hld needs no assembler at run time; the addl immediate is patched to
 * (plt slot - gp) for each import.
 *
 *     addl    r15 = 0, r1        // patched
 *     ;;
 *     ld8.acq r16 = [r15], 8     // entry point; step to the gp word
 *     mov     r14 = r1
 *     ;;
 *     ld8     r1 = [r15]         // the callee's gp
 *     mov     b6 = r16
 *     br.few  b6
 */
#define STUB_SIZE 32
static const uint8_t hld_stub_template[STUB_SIZE] = {
    0x0b, 0x78, 0x00, 0x02, 0x00, 0x24, 0x00, 0x41,
    0x3c, 0x70, 0x29, 0xc0, 0x01, 0x08, 0x00, 0x84,
    0x11, 0x08, 0x00, 0x1e, 0x18, 0x10, 0x60, 0x80,
    0x04, 0x80, 0x03, 0x00, 0x60, 0x00, 0x80, 0x00
};

/*
 * Reserve the dynamic sections and size them, before addresses are assigned.
 * Contents are filled afterwards by hld_fill_dynamic().
 */
int hld_alloc_dynamic(hld_link *L)
{
    osec *o;
    uint32_t nsym, nbucket;
    unsigned h;
    hld_gsym *g;
    hld_dso *d;
    lnkent *l;
    uint64_t nimp = 0;

    if (!L->dynamic) return 0;

    dynstr_add(L, "");                        /* index 0 is the empty string */

    /*
     * One dynamic symbol per import, after the mandatory null entry, plus
     * every defined global as an export: shared libraries bind against the
     * executable too (the C library, for instance, resolves `_end` and
     * `main` there), so an executable that exports nothing cannot be loaded.
     */
    nsym = 1;
    for (h = 0; h < HLD_SYMHASH; h++)
        for (g = L->hash[h]; g; g = g->next)
            if (g->kind == HLD_SYM_IMPORT) {
                g->dynidx = nsym++;
                g->plt_slot = nimp * 16;
                g->stub_off = nimp * STUB_SIZE;
                nimp++;
            } else if (g->kind == HLD_SYM_DEFINED || g->kind == HLD_SYM_ABS) {
                g->dynidx = nsym++;
            }
    L->ndynsym = nsym;
    L->nimports = nimp;

    if (L->shared) {
        /*
         * What the library calls itself. Anything linking against it records
         * this name, not the path it happened to be found at.
         */
        if (!L->soname) {
            const char *b = strrchr(L->out_path, '/');
            L->soname = b ? b + 1 : L->out_path;
        }
        L->soname_strx = dynstr_add(L, L->soname);
    }
    for (d = L->dsos; d; d = d->next)
        if (d->needed && !d->indirect) d->strx = dynstr_add(L, d->soname);
    for (h = 0; h < HLD_SYMHASH; h++)
        for (g = L->hash[h]; g; g = g->next)
            if (g->dynidx) g->strx = dynstr_add(L, g->name);

    if (!L->shared) {                 /* a library has no interpreter */
        o = osec_get(L, ".interp", SHT_PROGBITS, SHF_ALLOC);
        if (!o) return -1;
        o->size = sizeof hld_interp;
        o->align = 1;
        L->interpsec = o;
    }

    o = osec_get(L, ".dynsym", SHT_DYNSYM, SHF_ALLOC);
    if (!o) return -1;
    o->size = (uint64_t)nsym * SYM64_SIZE;
    o->align = 8;
    o->entsize = SYM64_SIZE;
    L->dynsymsec = o;

    o = osec_get(L, ".dynstr", SHT_STRTAB, SHF_ALLOC);
    if (!o) return -1;
    o->size = L->dynstr_len ? L->dynstr_len : 1;
    o->align = 1;
    L->dynstrsec = o;

    nbucket = nsym > 1 ? nsym : 1;            /* keep chains short */
    L->nbucket = nbucket;
    o = osec_get(L, ".hash", SHT_HASH, SHF_ALLOC);
    if (!o) return -1;
    o->size = (uint64_t)(2 + nbucket + nsym) * 4;
    o->align = 8;
    L->hashsec = o;

    if (nimp) {
        /* Import descriptors: {entry point, gp}, written by the loader. */
        o = osec_get(L, ".plt", SHT_PROGBITS,
                     SHF_ALLOC | SHF_WRITE | SHF_IA_64_SHORT);
        if (!o) return -1;
        o->size = nimp * 16;
        o->align = 16;
        o->entsize = 16;
        L->pltsec = o;

        o = osec_get(L, ".stub", SHT_PROGBITS,
                     SHF_ALLOC | SHF_EXECINSTR);
        if (!o) return -1;
        o->size = nimp * STUB_SIZE;
        o->align = 16;
        L->stubsec = o;
    }

    /*
     * Everything the loader has to write goes in one array: the import
     * descriptors, the linkage-table slots it has to fill, and the data words
     * holding such an address. Keeping them together means one DT_RELA span to
     * advertise, with no assumption about how the sections are laid out.
     *
     * A library needs this even with no imports at all. It can be mapped at an
     * address other than the one it was linked at, and its own exported
     * symbols can be interposed by whatever loaded it, so a table slot naming
     * one is the loader's to fill — not hld's.
     */
    L->ndltrel = 0;
    for (l = L->dlt; l; l = l->next)
        if (hld_dlt_needs_loader(L, l)) L->ndltrel++;

    if (nimp || L->ndltrel || L->ndynrel) {
        o = osec_get(L, ".rela.dyn", SHT_RELA, SHF_ALLOC);
        if (!o) return -1;
        o->size = (nimp + L->ndltrel + L->ndynrel) * RELA64_SIZE;
        o->align = 8;
        o->entsize = RELA64_SIZE;
        L->reladynsec = o;
    }

    o = osec_get(L, ".dynamic", SHT_DYNAMIC, SHF_ALLOC);
    if (!o) return -1;
    L->ndyntags = 13;                  /* the always-present set + DT_SONAME */
    for (d = L->dsos; d; d = d->next) if (d->needed) L->ndyntags++;
    if (nimp) L->ndyntags += 3;        /* PLT reserve, dld flags, load map */
    if (L->reladynsec) L->ndyntags += 3;   /* RELA, RELASZ, RELAENT */
    L->ndyntags += 6;          /* init/fini/preinit array tags, when present */
    o->size = (uint64_t)L->ndyntags * DYN64_SIZE;
    o->align = 8;
    o->entsize = DYN64_SIZE;
    L->dynamicsec = o;

    o = osec_get(L, ".sbss", SHT_NOBITS,
                 SHF_ALLOC | SHF_WRITE | SHF_IA_64_SHORT);
    if (!o) return -1;
    if (o->align < 16) o->align = 16;
    o->size = (o->size + 15) & ~15ULL;
    L->reserve_off = o->size;
    o->size += PLT_RESERVE_SIZE;
    L->loadmap_off = o->size;
    o->size += LOAD_MAP_SIZE;
    L->reservesec = o;
    return 0;
}

/*
 * Is this linkage-table slot the loader's to fill? Always when it names
 * something in another module; and in a shared library also when it names one
 * of its own exports, which can be interposed or moved.
 */
int hld_dlt_needs_loader(hld_link *L, const lnkent *l)
{
    if (!l->g) return 0;
    if (l->g->kind == HLD_SYM_IMPORT) return 1;
    return L->shared && l->g->kind == HLD_SYM_DEFINED;
}

/* Append one entry to the relocation array the loader walks at load time. */
static void reladyn_add(hld_link *L, uint64_t where, uint32_t dynidx,
                        uint32_t type, uint64_t addend)
{
    uint8_t *rp;

    if (!L->reladynsec || !L->reladynsec->data) return;
    if ((L->nreladyn + 1) * RELA64_SIZE > L->reladynsec->size) {
        L->reladyn_overflow = 1;   /* reported by the caller; never silent */
        return;
    }
    rp = L->reladynsec->data + L->nreladyn * RELA64_SIZE;
    st64(rp + 0, where);
    st64(rp + 8, ELF64_R_INFO(dynidx, type));
    st64(rp + 16, addend);
    L->nreladyn++;
}

/* Fill the dynamic sections; called once every address is final. */
int hld_fill_dynamic(hld_link *L)
{
    uint8_t *p;
    uint32_t i;
    uint64_t reserve_addr, loadmap_addr;
    size_t nd = 0;
    unsigned h;
    hld_gsym *g;
    hld_dso *d;
    uint32_t *bucket, *chain;

    if (!L->dynamic) return 0;

    reserve_addr = L->reservesec->addr + L->reserve_off;
    loadmap_addr = L->reservesec->addr + L->loadmap_off;

    if (L->interpsec && L->interpsec->data)
        memcpy(L->interpsec->data, hld_interp, sizeof hld_interp);
    if (L->dynstrsec->data && L->dynstr)
        memcpy(L->dynstrsec->data, L->dynstr, L->dynstr_len);

    /*
     * .dynsym: imports are undefined entries carrying the address they
     * resolved to at link time as a hint; exports name the section they are
     * defined in so libraries can bind to them.
     */
    if (L->dynsymsec->data)
        for (h = 0; h < HLD_SYMHASH; h++)
            for (g = L->hash[h]; g; g = g->next) {
                uint8_t *e;
                if (!g->dynidx) continue;
                e = L->dynsymsec->data + (size_t)g->dynidx * SYM64_SIZE;
                st32(e + 0, g->strx);
                e[5] = 0;
                if (g->kind == HLD_SYM_IMPORT) {
                    /* the platform's linker marks imports weak */
                    e[4] = ELF64_ST_INFO(STB_WEAK, STT_FUNC);
                    st16(e + 6, SHN_UNDEF);
                    st64(e + 8, g->hint);
                    st64(e + 16, 0);
                } else {
                    e[4] = ELF64_ST_INFO(g->bind ? g->bind : STB_GLOBAL,
                                         g->type);
                    st16(e + 6, (g->kind == HLD_SYM_DEFINED && g->in)
                                ? (uint16_t)g->in->out->shndx : SHN_ABS);
                    st64(e + 8, g->value);
                    st64(e + 16, g->size);
                }
            }

    /* .hash over the dynamic symbols */
    if (L->hashsec->data) {
        p = L->hashsec->data;
        st32(p + 0, L->nbucket);
        st32(p + 4, L->ndynsym);
        bucket = (uint32_t *)(void *)(p + 8);
        chain = bucket + L->nbucket;
        for (i = 0; i < L->nbucket + L->ndynsym; i++)
            st32(p + 8 + i * 4, 0);
        for (h = 0; h < HLD_SYMHASH; h++)
            for (g = L->hash[h]; g; g = g->next) {
                uint32_t b, prev;
                if (!g->dynidx) continue;
                b = elf_hash(g->name) % L->nbucket;
                prev = be32((uint8_t *)&bucket[b]);
                if (!prev) {
                    st32((uint8_t *)&bucket[b], g->dynidx);
                } else {                      /* append to the chain */
                    while (be32((uint8_t *)&chain[prev]))
                        prev = be32((uint8_t *)&chain[prev]);
                    st32((uint8_t *)&chain[prev], g->dynidx);
                }
            }
    }

    /*
     * Tell the loader which linkage-table slots name something in a shared
     * library. A plain address slot takes DIR64; a descriptor slot takes
     * FPTR64, which asks the loader for the canonical descriptor of a
     * function it owns — hld cannot build that one itself, since the entry
     * point and gp both belong to the other module. Without these the slot
     * stays as hld left it and the program dereferences a null pointer the
     * first time it uses the symbol.
     */
    {
        lnkent *l;
        size_t n;
        for (l = L->dlt; l; l = l->next) {
            if (!hld_dlt_needs_loader(L, l)) continue;
            reladyn_add(L, L->dltsec->addr + l->slot, l->g->dynidx,
                        l->kind == HLD_DLT_FPTR ? R_IA64_FPTR64MSB
                                                : R_IA64_DIR64MSB, 0);
        }
        /* And the data words that hold another module's address. */
        for (n = 0; n < L->ndynrel; n++) {
            dynrel *dr = &L->dynrels[n];
            reladyn_add(L, dr->in->out->addr + dr->in->out_off + dr->off,
                        dr->g->dynidx, dr->type, dr->addend);
        }
    }

    /* Import descriptors, their relocations, and the call stubs. */
    if (L->nimports) {
        uint64_t nfilled = 0;
        for (h = 0; h < HLD_SYMHASH; h++)
            for (g = L->hash[h]; g; g = g->next) {
                uint64_t plt_addr;
                uint8_t *sp;

                if (g->kind != HLD_SYM_IMPORT) continue;
                nfilled++;
                plt_addr = L->pltsec->addr + g->plt_slot;

                /*
                 * Pre-fill the descriptor the way the platform's linker does:
                 * an entry point plus this module's gp. The loader overwrites
                 * both words when it binds the symbol; leaving them zero makes
                 * it reject the image.
                 */
                if (L->pltsec->data) {
                    st64(L->pltsec->data + g->plt_slot, g->hint);
                    st64(L->pltsec->data + g->plt_slot + 8, L->gp);
                }
                reladyn_add(L, plt_addr, g->dynidx, R_IA64_IPLTMSB, 0);
                if (L->stubsec->data) {
                    sp = L->stubsec->data + g->stub_off;
                    memcpy(sp, hld_stub_template, STUB_SIZE);
                    /* addl r15 = (descriptor - gp), r1 */
                    if (hld_ia64_install_value(sp, 0,
                                               plt_addr - L->gp,
                                               R_IA64_IMM22) != HLD_PATCH_OK) {
                        snprintf(L->err, HLD_ERRSZ,
                                 "import `%s': linkage table is too far from gp",
                                 g->name);
                        return -1;
                    }
                }
                g->value = L->stubsec->addr + g->stub_off;  /* calls go here */
            }
        /*
         * A descriptor left unwritten is a call that jumps to zero, and a
         * hole in the relocation array the loader rejects outright. It can
         * only happen if a symbol stopped being an import after the linkage
         * table was sized, which is this linker's mistake, not a bad input.
         */
        if (nfilled != L->nimports) {
            snprintf(L->err, HLD_ERRSZ,
                     "internal: %llu import descriptors reserved, %llu written",
                     U(L->nimports), U(nfilled));
            return -1;
        }
    }

    if (L->dynamicsec->data) {
        p = L->dynamicsec->data;
        /*
         * Dropping a tag because the array was sized too small is silent and
         * ruinous: the loader simply never learns what it was not told. The
         * count is a prediction made before the contents are known, so check
         * it rather than trust it.
         */
#define DYN(tag, val) do { \
            if (nd >= L->ndyntags) { \
                snprintf(L->err, HLD_ERRSZ, \
                         "internal: .dynamic needs more than the %u entries " \
                         "reserved for it", (unsigned)L->ndyntags); \
                return -1; \
            } \
            st64(p + nd * DYN64_SIZE, (uint64_t)(tag)); \
            st64(p + nd * DYN64_SIZE + 8, (uint64_t)(val)); \
            nd++; \
        } while (0)
        for (d = L->dsos; d; d = d->next)
            if (d->needed && !d->indirect) DYN(DT_NEEDED, d->strx);
        if (L->shared) DYN(DT_SONAME, L->soname_strx);
        /*
         * Ask for immediate binding: hld does not emit the lazy-resolution
         * trampoline, so every import must be bound before control reaches
         * the program.
         */
        DYN(DT_HP_DLD_FLAGS, DT_HP_DF_BIND_NOW);
        DYN(DT_PLTGOT, L->gp);
        DYN(DT_HASH, L->hashsec->addr);
        DYN(DT_STRTAB, L->dynstrsec->addr);
        DYN(DT_SYMTAB, L->dynsymsec->addr);
        DYN(DT_STRSZ, L->dynstrsec->size);
        DYN(DT_SYMENT, SYM64_SIZE);
        /*
     * Advertise exactly the relocations that were written. Reserving more
     * than are used would otherwise leave zeroed entries in the array, and
     * the loader reads those as relocation type 0 and refuses the image.
     */
    if (L->reladynsec) {
        if (L->reladyn_overflow) {
            snprintf(L->err, HLD_ERRSZ,
                     "internal: more dynamic relocations than the %llu reserved",
                     U(L->reladynsec->size / RELA64_SIZE));
            return -1;
        }
        L->reladynsec->size = (uint64_t)L->nreladyn * RELA64_SIZE;
    }
    if (L->reladynsec) {
            /*
             * Advertise the relocations once, through DT_RELA only.
             * Naming the same array again as DT_JMPREL — which is what the
             * platform's linker does for lazy binding — makes the loader
             * process it a second time and reject the image, since hld asks
             * for immediate binding and the entries are already applied.
             */
            DYN(DT_RELA, L->reladynsec->addr);
            DYN(DT_RELASZ, L->reladynsec->size);
            DYN(DT_RELAENT, RELA64_SIZE);
        }
        /*
         * Static constructors run because the loader is told where the
         * initializer array is; without these the array is laid out and
         * never walked, and a C++ program starts with its globals
         * unconstructed — including the runtime's own.
         */
        {
            osec *a;
            a = osec_find_pub(L, ".init_array");
            if (a && a->size) {
                DYN(DT_INIT_ARRAY, a->addr);
                DYN(DT_INIT_ARRAYSZ, a->size);
            }
            a = osec_find_pub(L, ".fini_array");
            if (a && a->size) {
                DYN(DT_FINI_ARRAY, a->addr);
                DYN(DT_FINI_ARRAYSZ, a->size);
            }
            a = osec_find_pub(L, ".preinit_array");
            if (a && a->size) {
                DYN(DT_PREINIT_ARRAY, a->addr);
                DYN(DT_PREINIT_ARRAYSZ, a->size);
            }
        }
        DYN(DT_IA_64_PLT_RESERVE, reserve_addr);
        /*
         * The load-map word is the loader's to write, and it is an
         * executable's business: the platform's linker emits this tag for a
         * program and not for a library.
         */
        if (!L->shared) DYN(DT_HP_LOAD_MAP, loadmap_addr);
        DYN(DT_FLAGS, 0);
        DYN(DT_NULL, 0);
#undef DYN
    }
    return 0;
}

/* ---- shared library inputs -------------------------------------------- */

static unsigned dso_hash(const char *s)
{
    unsigned h = 5381;
    while (*s) h = h * 33 + (unsigned char)*s++;
    return h % 1021;
}

/* Look up an exported symbol across the libraries, in command-line order. */
static dsosym *dso_lookup(hld_link *L, const char *name, hld_dso **which)
{
    hld_dso *d;
    dsosym *s;

    for (d = L->dsos; d; d = d->next)
        for (s = d->hash[dso_hash(name)]; s; s = s->next)
            if (strcmp(s->name, name) == 0) {
                *which = d;
                return s;
            }
    return NULL;
}

int hld_add_libpath(hld_link *L, const char *dir)
{
    if (L->nlibpaths == L->libpaths_cap) {
        size_t nc = L->libpaths_cap ? L->libpaths_cap * 2 : 8;
        char **np = realloc(L->libpaths, nc * sizeof *np);
        if (!np) return -1;
        L->libpaths = np;
        L->libpaths_cap = nc;
    }
    L->libpaths[L->nlibpaths++] = (char *)dir;
    return 0;
}


/*
 * Resolve -lNAME against the -L list. Each directory is tried in turn for the
 * shared forms and then the archive, so a directory holding both yields the
 * shared one, as every Unix linker does. An archive is searched immediately:
 * it must see exactly the symbols undefined at its position.
 */
int hld_find_library(hld_link *L, const char *name, hld_archive **ar_out)
{
    char path[1024];
    size_t i;
    FILE *f;
    int want_ar_first = (L->libmode == HLD_LIB_ARCHIVE
                         || L->libmode == HLD_LIB_ARCHIVE_SHARED);
    int allow_shared = (L->libmode != HLD_LIB_ARCHIVE);
    int allow_ar = (L->libmode != HLD_LIB_SHARED);

    if (ar_out) *ar_out = NULL;
    for (i = 0; i < L->nlibpaths; i++) {
        int try;
        /*
         * -a decides which kind is preferred, and whether the other kind is
         * acceptable at all. gcc uses this for -static-libstdc++, which asks
         * for the archive of one library in the middle of an otherwise
         * shared link.
         */
        for (try = 0; try < 2; try++) {
            int archive = want_ar_first ? (try == 0) : (try == 1);

            if (archive) {
                if (!allow_ar) continue;
                snprintf(path, sizeof path, "%s/lib%s.a", L->libpaths[i], name);
                f = fopen(path, "rb");
                if (f) {
                    hld_archive *ar;
                    fclose(f);
                    if (hld_archive_open(L, path, &ar) < 0) return -1;
                    if (ar_out) *ar_out = ar;
                    return hld_archive_search(L, ar, NULL);
                }
            } else {
                if (!allow_shared) continue;
                snprintf(path, sizeof path, "%s/lib%s.so", L->libpaths[i], name);
                f = fopen(path, "rb");
                if (f) { fclose(f); return hld_add_dso(L, path); }
                /* HP names the C library libc.so.1 rather than libc.so */
                snprintf(path, sizeof path, "%s/lib%s.so.1",
                         L->libpaths[i], name);
                f = fopen(path, "rb");
                if (f) { fclose(f); return hld_add_dso(L, path); }
            }
        }
    }
    snprintf(L->err, HLD_ERRSZ, "cannot find library -l%s", name);
    return -1;
}

/*
 * A library's own dependencies are named in its DT_NEEDED and satisfy
 * references the same way it does — the C++ unwinder, for instance, reaches
 * the register-context routines through libuca, which no link line mentions.
 * They are loaded for resolution but never recorded as this image's own
 * NEEDED: the loader follows the chain itself, which is what the platform's
 * linker leaves it to do.
 */
static int add_dso(hld_link *L, const char *path, int indirect);

int hld_add_dso(hld_link *L, const char *path)
{
    return add_dso(L, path, 0);
}

/* Already loaded, by soname or by path? */
static int dso_loaded(hld_link *L, const char *soname)
{
    hld_dso *d;
    for (d = L->dsos; d; d = d->next)
        if (d->soname && strcmp(d->soname, soname) == 0) return 1;
    return 0;
}

static int add_dso(hld_link *L, const char *path, int indirect)
{
    char err[HLD_ERRSZ];
    hld_elf *e;
    hld_dso *d;
    uint32_t i;
    const char *base;
    char needed[16][128];
    size_t nneeded = 0;

    e = hld_elf_load(path, err);
    if (!e) { snprintf(L->err, HLD_ERRSZ, "%s", err); return -1; }
    if (e->eh.type != ET_DYN) {
        snprintf(L->err, HLD_ERRSZ, "%s: not a shared library", path);
        hld_elf_free(e);
        return -1;
    }
    d = calloc(1, sizeof *d);
    if (!d) { hld_elf_free(e); snprintf(L->err, HLD_ERRSZ, "out of memory"); return -1; }
    d->elf = e;

    base = strrchr(path, '/');
    d->soname = base ? base + 1 : path;

    for (i = 1; i < e->eh.shnum; i++) {
        hld_shdr *sh = &e->shdrs[i];

        if (sh->type == SHT_DYNAMIC) {          /* prefer the recorded soname */
            size_t nd, k;
            hld_dyn *dyn = hld_read_dyn(e, sh, &nd, err);
            uint64_t straddr = 0;
            if (dyn) {
                for (k = 0; k < nd; k++)
                    if (dyn[k].tag == DT_STRTAB) straddr = dyn[k].val;
                for (k = 0; k < nd; k++) {
                    const hld_shdr *st;
                    const char *nm;
                    if (!straddr) continue;
                    if (dyn[k].tag != DT_SONAME && dyn[k].tag != DT_NEEDED)
                        continue;
                    st = hld_sec_by_addr(e, straddr);
                    if (!st || st->type != SHT_STRTAB) continue;
                    nm = hld_strtab_str(e, (uint32_t)(st - e->shdrs),
                                        dyn[k].val);
                    if (!nm || !nm[0]) continue;
                    if (dyn[k].tag == DT_SONAME) {
                        d->soname = nm;
                    } else if (nneeded < sizeof needed / sizeof needed[0]) {
                        snprintf(needed[nneeded], sizeof needed[0], "%s", nm);
                        nneeded++;
                    }
                }
                free(dyn);
            }
        }
        if (sh->type != SHT_DYNSYM) continue;
        {
            size_t n, k;
            hld_sym *syms = hld_read_syms(e, sh, &n, err);
            if (!syms) continue;
            for (k = 0; k < n; k++) {
                hld_sym *sy = &syms[k];
                dsosym *ds;
                unsigned h;

                if (sy->shndx == SHN_UNDEF || !sy->name[0]) continue;
                if (ELF64_ST_BIND(sy->info) == STB_LOCAL) continue;
                h = dso_hash(sy->name);
                ds = calloc(1, sizeof *ds);
                if (!ds) continue;
                ds->name = sy->name;    /* points into the library's image */
                ds->value = sy->value;
                ds->next = d->hash[h];
                d->hash[h] = ds;
            }
            free(syms);
        }
    }

    d->indirect = indirect;
    if (!L->dsos) { L->dsos = d; L->dso_tail = &d->next; }
    else { *L->dso_tail = d; L->dso_tail = &d->next; }
    L->ndsos++;
    L->dynamic = 1;                     /* using a library implies dynamic */

    /* Its own dependencies, so what they define can satisfy this link too. */
    {
        size_t k;
        for (k = 0; k < nneeded; k++) {
            char p2[1024];
            size_t li;
            FILE *f;
            if (dso_loaded(L, needed[k])) continue;
            for (li = 0; li < L->nlibpaths; li++) {
                size_t dl = strlen(L->libpaths[li]);
                if (dl + strlen(needed[k]) + 2 > sizeof p2) continue;
                memcpy(p2, L->libpaths[li], dl);
                p2[dl] = '/';
                strcpy(p2 + dl + 1, needed[k]);
                f = fopen(p2, "rb");
                if (f) {
                    fclose(f);
                    if (add_dso(L, p2, 1) < 0) return -1;
                    break;
                }
            }
        }
    }

    /*
     * Bind what is undefined right now, at this library's position on the
     * command line, so an archive searched later is not asked for a symbol
     * this library has already supplied. A library stays available for
     * symbols that come up afterwards, which hld_bind_imports() sweeps up.
     */
    return hld_bind_imports(L);
}

/* Bind still-undefined symbols to library exports. */
int hld_bind_imports(hld_link *L)
{
    unsigned h;
    hld_gsym *g;

    for (h = 0; h < HLD_SYMHASH; h++)
        for (g = L->hash[h]; g; g = g->next) {
            hld_dso *d = NULL;
            dsosym *s;

            if (g->kind != HLD_SYM_UNDEF) continue;
            /*
             * `__gp', `_end' and the rest name this module's own layout.
             * A library built by this linker exports them, so without this
             * a program would import one, be given a descriptor in the
             * linkage table, and then have the symbol defined locally at
             * layout after all -- leaving that descriptor unwritten.
             */
            if (hld_is_linker_symbol(g->name)) continue;
            s = dso_lookup(L, g->name, &d);
            if (!s) continue;
            g->kind = HLD_SYM_IMPORT;
            g->dso = d;
            g->hint = s->value;
            d->needed = 1;
            L->nimports++;
        }
    return 0;
}
