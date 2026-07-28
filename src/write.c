/*
 * write.c — emit an HP-UX/IPF LP64 ET_EXEC image.
 *
 * A static image is two LOAD segments plus PT_PHDR; a dynamic one adds
 * PT_INTERP and PT_DYNAMIC. The file layout mirrors what HP ld produces — the text segment is mapped from
 * file offset 0 so the ELF header and program headers live in its first
 * page, and the data segment's file offset stays congruent to its vaddr
 * modulo HLD_SEG_ALIGN.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"

#define U(x) ((unsigned long long)(x))

typedef struct {
    char *buf;
    size_t len, cap;
} strtab;

static int strtab_init(strtab *s)
{
    s->cap = 256;
    s->len = 1;                 /* index 0 is the empty string */
    s->buf = calloc(1, s->cap);
    return s->buf ? 0 : -1;
}

static uint32_t strtab_add(strtab *s, const char *str)
{
    size_t n = strlen(str) + 1;
    uint32_t off;

    if (s->len + n > s->cap) {
        size_t nc = s->cap * 2;
        char *nb;
        while (nc < s->len + n) nc *= 2;
        nb = realloc(s->buf, nc);
        if (!nb) return 0;
        memset(nb + s->cap, 0, nc - s->cap);
        s->buf = nb;
        s->cap = nc;
    }
    off = (uint32_t)s->len;
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    return off;
}

/* Grow-on-demand output image. */
typedef struct {
    uint8_t *p;
    size_t len, cap;
} image;

static int img_need(image *im, size_t end)
{
    if (end > im->cap) {
        size_t nc = im->cap ? im->cap : 4096;
        uint8_t *np;
        while (nc < end) nc *= 2;
        np = realloc(im->p, nc);
        if (!np) return -1;
        memset(np + im->cap, 0, nc - im->cap);
        im->p = np;
        im->cap = nc;
    }
    if (end > im->len) im->len = end;
    return 0;
}

static int img_write(image *im, uint64_t off, const void *data, size_t n)
{
    if (img_need(im, (size_t)off + n) < 0) return -1;
    if (n) memcpy(im->p + off, data, n);
    return 0;
}

static int img_zero(image *im, uint64_t off, size_t n)
{
    return img_need(im, (size_t)off + n);
}

static void put_phdr(uint8_t *p, uint32_t type, uint32_t flags, uint64_t off,
                     uint64_t vaddr, uint64_t filesz, uint64_t memsz,
                     uint64_t align)
{
    st32(p + 0, type);
    st32(p + 4, flags);
    st64(p + 8, off);
    st64(p + 16, vaddr);
    st64(p + 24, 0);            /* p_paddr is always 0 on this platform */
    st64(p + 32, filesz);
    st64(p + 40, memsz);
    st64(p + 48, align);
}

static void put_shdr(uint8_t *p, uint32_t name, uint32_t type, uint64_t flags,
                     uint64_t addr, uint64_t off, uint64_t size, uint32_t link,
                     uint32_t info, uint64_t align, uint64_t entsize)
{
    st32(p + 0, name);
    st32(p + 4, type);
    st64(p + 8, flags);
    st64(p + 16, addr);
    st64(p + 24, off);
    st64(p + 32, size);
    st32(p + 40, link);
    st32(p + 44, info);
    st64(p + 48, align);
    st64(p + 56, entsize);
}

static void put_sym(uint8_t *p, uint32_t name, uint8_t info, uint8_t other,
                    uint16_t shndx, uint64_t value, uint64_t size)
{
    st32(p + 0, name);
    p[4] = info;
    p[5] = other;
    st16(p + 6, shndx);
    st64(p + 8, value);
    st64(p + 16, size);
}

int hld_write_exec(hld_link *L)
{
    image im;
    strtab shstr, str;
    osec *o;
    uint8_t eh[EHDR64_SIZE], ph[PHDR64_SIZE];
    uint64_t phoff = EHDR64_SIZE, shoff;
    uint32_t nphdr = L->nphdr ? L->nphdr : 3, nsec;
    uint64_t off;
    uint8_t *symbuf = NULL;
    size_t nsym = 0, symcap = 0, symsz;
    uint32_t nlocal;
    unsigned h;
    hld_gsym *g;
    FILE *f;
    int rc = -1;

    memset(&im, 0, sizeof im);
    if (strtab_init(&shstr) < 0 || strtab_init(&str) < 0) {
        snprintf(L->err, HLD_ERRSZ, "out of memory");
        return -1;
    }

    /* --- section contents ------------------------------------------------ */
    for (o = L->osecs; o; o = o->next) {
        if (o->type == SHT_NOBITS || !o->size) continue;
        if (img_write(&im, o->off, o->data, (size_t)o->size) < 0) goto oom;
    }
    /*
     * The file must physically extend to the data segment's file offset even
     * when that segment has no file content: the kernel's loader validates
     * p_offset against the file size and refuses the image ("Exec format
     * error") if it points past the end. Proven by experiment on 11.23 —
     * padding an otherwise-identical rejected image makes it run.
     */
    if (img_zero(&im, L->data_off + L->data_filesz, 0) < 0)
        goto oom;
    off = im.len;

    /*
     * Assign output section indices before emitting symbols: a symbol's
     * st_shndx names the section it is defined in, so the numbering has to
     * exist first.
     */
    nsec = 1;                                     /* SHT_NULL */
    for (o = L->osecs; o; o = o->next) o->shndx = nsec++;

    /* --- symbol table ---------------------------------------------------- */
    /* index 0 is the null entry; locals first (we emit only the null local) */
    symcap = 64;
    symbuf = calloc(symcap, SYM64_SIZE);
    if (!symbuf) goto oom;
    memset(symbuf, 0, SYM64_SIZE);
    nsym = 1;
    nlocal = 1;

    for (h = 0; h < HLD_SYMHASH; h++) {
        for (g = L->hash[h]; g; g = g->next) {
            uint16_t shndx = SHN_ABS;
            uint32_t nameoff;
            uint8_t info;

            if (g->kind != HLD_SYM_DEFINED && g->kind != HLD_SYM_ABS) continue;
            if (nsym == symcap) {
                uint8_t *nb = realloc(symbuf, symcap * 2 * SYM64_SIZE);
                if (!nb) goto oom;
                memset(nb + symcap * SYM64_SIZE, 0, symcap * SYM64_SIZE);
                symbuf = nb;
                symcap *= 2;
            }
            if (g->kind == HLD_SYM_DEFINED && g->in)
                shndx = (uint16_t)g->in->out->shndx;
            nameoff = strtab_add(&str, g->name);
            info = ELF64_ST_INFO(g->bind ? g->bind : STB_GLOBAL, g->type);
            put_sym(symbuf + nsym * SYM64_SIZE, nameoff, info, g->other,
                    shndx, g->value, g->size);
            nsym++;
        }
    }
    symsz = nsym * SYM64_SIZE;

    /* --- section header table: .symtab, .strtab, .shstrtab follow ------- */
    {
        uint32_t sym_ndx = nsec++, str_ndx = nsec++, shstr_ndx = nsec++;
        uint64_t symoff, stroff, shstroff;
        uint32_t nullname = 0;
        uint8_t *shtab;

        (void)nullname;
        off = (off + 7) & ~7ULL;
        symoff = off;
        if (img_write(&im, symoff, symbuf, symsz) < 0) goto oom;
        off = symoff + symsz;
        stroff = off;
        if (img_write(&im, stroff, str.buf, str.len) < 0) goto oom;
        off = stroff + str.len;

        /* section header string table */
        for (o = L->osecs; o; o = o->next)
            o->entsize = o->entsize;              /* names added below */
        {
            /* record name offsets in a parallel pass */
            uint32_t *names = calloc(L->nosecs + 4, sizeof *names);
            uint32_t k = 0;
            uint32_t n_sym, n_str, n_shstr;

            if (!names) goto oom;
            for (o = L->osecs; o; o = o->next) names[k++] = strtab_add(&shstr, o->name);
            n_sym = strtab_add(&shstr, ".symtab");
            n_str = strtab_add(&shstr, ".strtab");
            n_shstr = strtab_add(&shstr, ".shstrtab");

            shstroff = off;
            if (img_write(&im, shstroff, shstr.buf, shstr.len) < 0) {
                free(names);
                goto oom;
            }
            off = shstroff + shstr.len;

            /* --- section headers ---------------------------------------- */
            off = (off + 7) & ~7ULL;
            shoff = off;
            shtab = calloc(nsec, SHDR64_SIZE);
            if (!shtab) { free(names); goto oom; }

            k = 0;
            for (o = L->osecs; o; o = o->next, k++) {
                uint32_t link = 0;
                /* .dynsym/.dynamic/.hash name the dynamic string/symbol table */
                if (L->dynamic && L->dynstrsec
                    && (o == L->dynsymsec || o == L->dynamicsec))
                    link = L->dynstrsec->shndx;
                if (L->dynamic && L->dynsymsec && o == L->hashsec)
                    link = L->dynsymsec->shndx;
                put_shdr(shtab + o->shndx * SHDR64_SIZE, names[k], o->type,
                         o->flags, o->addr, o->off, o->size, link, 0, o->align,
                         o->entsize);
            }
            put_shdr(shtab + sym_ndx * SHDR64_SIZE, n_sym, SHT_SYMTAB, 0, 0,
                     symoff, symsz, str_ndx, nlocal, 8, SYM64_SIZE);
            put_shdr(shtab + str_ndx * SHDR64_SIZE, n_str, SHT_STRTAB, 0, 0,
                     stroff, str.len, 0, 0, 1, 0);
            put_shdr(shtab + shstr_ndx * SHDR64_SIZE, n_shstr, SHT_STRTAB, 0, 0,
                     shstroff, shstr.len, 0, 0, 1, 0);
            free(names);

            if (img_write(&im, shoff, shtab, (size_t)nsec * SHDR64_SIZE) < 0) {
                free(shtab);
                goto oom;
            }
            free(shtab);
        }

        /* --- program headers ------------------------------------------- */
        put_phdr(ph, PT_PHDR, PF_R, phoff, HLD_TEXT_BASE + phoff,
                 (uint64_t)nphdr * PHDR64_SIZE, (uint64_t)nphdr * PHDR64_SIZE, 8);
        if (img_write(&im, phoff, ph, PHDR64_SIZE) < 0) goto oom;

        put_phdr(ph, PT_LOAD, PF_R | PF_X, 0, HLD_TEXT_BASE, L->text_filesz,
                 L->text_filesz, 0x10);
        if (img_write(&im, phoff + PHDR64_SIZE, ph, PHDR64_SIZE) < 0) goto oom;

        put_phdr(ph, PT_LOAD, PF_R | PF_W, L->data_off, L->data_addr,
                 L->data_filesz, L->data_memsz, 0x10);
        if (img_write(&im, phoff + 2 * PHDR64_SIZE, ph, PHDR64_SIZE) < 0) goto oom;

        if (L->dynamic) {
            put_phdr(ph, PT_INTERP, PF_R, L->interpsec->off,
                     L->interpsec->addr, L->interpsec->size,
                     L->interpsec->size, 1);
            if (img_write(&im, phoff + 3 * PHDR64_SIZE, ph, PHDR64_SIZE) < 0)
                goto oom;
            put_phdr(ph, PT_DYNAMIC, PF_R, L->dynamicsec->off,
                     L->dynamicsec->addr, L->dynamicsec->size,
                     L->dynamicsec->size, 8);
            if (img_write(&im, phoff + 4 * PHDR64_SIZE, ph, PHDR64_SIZE) < 0)
                goto oom;
        }

        /* --- ELF header -------------------------------------------------- */
        memset(eh, 0, sizeof eh);
        memcpy(eh, ELFMAG, 4);
        eh[4] = ELFCLASS64;
        eh[5] = ELFDATA2MSB;
        eh[6] = EV_CURRENT;
        eh[7] = ELFOSABI_HPUX;
        eh[8] = HPUX_ABIVERSION;
        st16(eh + 16, ET_EXEC);
        st16(eh + 18, EM_IA_64);
        st32(eh + 20, EV_CURRENT);
        st64(eh + 24, L->entry);
        st64(eh + 32, phoff);
        st64(eh + 40, shoff);
        st32(eh + 48, EF_IA_64_BE | EF_IA_64_ABI64
                      | (L->trapnil ? EF_IA_64_TRAPNIL : 0));
        st16(eh + 52, EHDR64_SIZE);
        st16(eh + 54, PHDR64_SIZE);
        st16(eh + 56, (uint16_t)nphdr);
        st16(eh + 58, SHDR64_SIZE);
        st16(eh + 60, (uint16_t)nsec);
        st16(eh + 62, (uint16_t)shstr_ndx);
        if (img_write(&im, 0, eh, EHDR64_SIZE) < 0) goto oom;
    }

    /* --- out ------------------------------------------------------------- */
    f = fopen(L->out_path, "wb");
    if (!f) {
        snprintf(L->err, HLD_ERRSZ, "%s: cannot create output", L->out_path);
        goto done;
    }
    if (fwrite(im.p, 1, im.len, f) != im.len) {
        snprintf(L->err, HLD_ERRSZ, "%s: write failed", L->out_path);
        fclose(f);
        goto done;
    }
    fclose(f);
    rc = 0;
    goto done;

oom:
    snprintf(L->err, HLD_ERRSZ, "out of memory");
done:
    free(symbuf);
    free(shstr.buf);
    free(str.buf);
    free(im.p);
    return rc;
}
