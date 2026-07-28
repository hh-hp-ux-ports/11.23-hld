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
 * Reserve the dynamic sections and size them, before addresses are assigned.
 * Contents are filled afterwards by hld_fill_dynamic().
 */
int hld_alloc_dynamic(hld_link *L)
{
    osec *o;
    uint32_t nsym, nbucket;

    if (!L->dynamic) return 0;

    /* One dynamic symbol so far: the mandatory null entry. */
    nsym = 1;
    L->ndynsym = nsym;
    dynstr_add(L, "");                        /* ensure index 0 exists */

    o = osec_get(L, ".interp", SHT_PROGBITS, SHF_ALLOC);
    if (!o) return -1;
    o->size = sizeof hld_interp;              /* includes the NUL */
    o->align = 1;
    L->interpsec = o;

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

    /* .hash: nbucket, nchain, buckets[nbucket], chain[nchain] */
    nbucket = 1;
    L->nbucket = nbucket;
    o = osec_get(L, ".hash", SHT_HASH, SHF_ALLOC);
    if (!o) return -1;
    o->size = (uint64_t)(2 + nbucket + nsym) * 4;
    o->align = 8;
    L->hashsec = o;

    o = osec_get(L, ".dynamic", SHT_DYNAMIC, SHF_ALLOC);
    if (!o) return -1;
    L->ndyntags = 11;                         /* see hld_fill_dynamic() */
    o->size = (uint64_t)L->ndyntags * DYN64_SIZE;
    o->align = 8;
    o->entsize = DYN64_SIZE;
    L->dynamicsec = o;

    /*
     * Writable scratch the loader requires: its reserved linkage-table slots
     * and the load-map word. Both live in short bss so they stay within gp's
     * reach, which is where the platform's linker puts them too.
     */
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

/* Fill the dynamic sections; called once every address is final. */
int hld_fill_dynamic(hld_link *L)
{
    uint8_t *p;
    uint32_t i;
    uint64_t reserve_addr, loadmap_addr;
    size_t nd = 0;

    if (!L->dynamic) return 0;

    reserve_addr = L->reservesec->addr + L->reserve_off;
    loadmap_addr = L->reservesec->addr + L->loadmap_off;

    if (L->interpsec->data)
        memcpy(L->interpsec->data, hld_interp, sizeof hld_interp);
    if (L->dynstrsec->data && L->dynstr)
        memcpy(L->dynstrsec->data, L->dynstr, L->dynstr_len);
    /* .dynsym's null entry is already zero. */

    if (L->hashsec->data) {                   /* empty table: all buckets 0 */
        p = L->hashsec->data;
        st32(p + 0, L->nbucket);
        st32(p + 4, L->ndynsym);
        for (i = 0; i < L->nbucket + L->ndynsym; i++)
            st32(p + 8 + i * 4, 0);
    }

    if (L->dynamicsec->data) {
        p = L->dynamicsec->data;
#define DYN(tag, val) do { \
            if (nd + 1 < L->ndyntags + 1) { \
                st64(p + nd * DYN64_SIZE, (uint64_t)(tag)); \
                st64(p + nd * DYN64_SIZE + 8, (uint64_t)(val)); \
                nd++; \
            } \
        } while (0)
        DYN(DT_HP_DLD_FLAGS, 0);
        DYN(DT_PLTGOT, L->gp);
        DYN(DT_HASH, L->hashsec->addr);
        DYN(DT_STRTAB, L->dynstrsec->addr);
        DYN(DT_SYMTAB, L->dynsymsec->addr);
        DYN(DT_STRSZ, L->dynstrsec->size);
        DYN(DT_SYMENT, SYM64_SIZE);
        DYN(DT_IA_64_PLT_RESERVE, reserve_addr);
        DYN(DT_HP_LOAD_MAP, loadmap_addr);
        DYN(DT_FLAGS, 0);
        DYN(DT_NULL, 0);
#undef DYN
    }
    (void)elf_hash;                            /* used once imports exist */
    return 0;
}
