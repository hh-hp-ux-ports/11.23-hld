/*
 * write.c — emit an HP-UX/IPF LP64 image, executable or shared library.
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
#include <sys/types.h>
#include <sys/stat.h>

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

static void put_phdr(int e32, uint8_t *p, uint32_t type, uint32_t flags,
                     uint64_t off, uint64_t vaddr, uint64_t filesz,
                     uint64_t memsz, uint64_t align)
{
    st32(p + 0, type);
    if (e32) {
        /* ELF32 keeps p_flags LAST; ELF64 moved it up to offset 4. */
        st32(p + 4, (uint32_t)off);
        st32(p + 8, (uint32_t)vaddr);
        st32(p + 12, 0);        /* p_paddr is always 0 on this platform */
        st32(p + 16, (uint32_t)filesz);
        st32(p + 20, (uint32_t)memsz);
        st32(p + 24, flags);
        st32(p + 28, (uint32_t)align);
        return;
    }
    st32(p + 4, flags);
    st64(p + 8, off);
    st64(p + 16, vaddr);
    st64(p + 24, 0);            /* p_paddr is always 0 on this platform */
    st64(p + 32, filesz);
    st64(p + 40, memsz);
    st64(p + 48, align);
}

static void put_shdr(int e32, uint8_t *p, uint32_t name, uint32_t type,
                     uint64_t flags, uint64_t addr, uint64_t off,
                     uint64_t size, uint32_t link, uint32_t info,
                     uint64_t align, uint64_t entsize)
{
    st32(p + 0, name);
    st32(p + 4, type);
    if (e32) {
        st32(p + 8, (uint32_t)flags);
        st32(p + 12, (uint32_t)addr);
        st32(p + 16, (uint32_t)off);
        st32(p + 20, (uint32_t)size);
        st32(p + 24, link);
        st32(p + 28, info);
        st32(p + 32, (uint32_t)align);
        st32(p + 36, (uint32_t)entsize);
        return;
    }
    st64(p + 8, flags);
    st64(p + 16, addr);
    st64(p + 24, off);
    st64(p + 32, size);
    st32(p + 40, link);
    st32(p + 44, info);
    st64(p + 48, align);
    st64(p + 56, entsize);
}

static void put_sym(int e32, uint8_t *p, uint32_t name, uint8_t info,
                    uint8_t other, uint16_t shndx, uint64_t value,
                    uint64_t size)
{
    st32(p + 0, name);
    if (e32) {
        /* ELF32 orders these name/value/size/info/other/shndx. */
        st32(p + 4, (uint32_t)value);
        st32(p + 8, (uint32_t)size);
        p[12] = info;
        p[13] = other;
        st16(p + 14, shndx);
        return;
    }
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
    const int e32 = L->elf32;
    const uint64_t ehsz  = e32 ? EHDR32_SIZE : EHDR64_SIZE;
    const uint64_t phsz  = e32 ? PHDR32_SIZE : PHDR64_SIZE;
    const uint64_t shsz  = e32 ? SHDR32_SIZE : SHDR64_SIZE;
    const uint64_t symsz1 = e32 ? SYM32_SIZE : SYM64_SIZE;
    uint8_t eh[EHDR64_SIZE], ph[PHDR64_SIZE];
    uint64_t phoff = ehsz, shoff;
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

    /* Section indices were assigned during layout (see hld_layout). */
    nsec = 1;                                     /* SHT_NULL */
    for (o = L->osecs; o; o = o->next) nsec++;

    /* --- symbol table ---------------------------------------------------- */
    /* index 0 is the null entry, and ELF wants every local before any global */
    symcap = 64;
    symbuf = calloc(symcap, symsz1);
    if (!symbuf) goto oom;
    memset(symbuf, 0, (size_t)symsz1);
    nsym = 1;

    /*
     * Locals, from each object's own symbol table -- they are never interned
     * into L->hash, which holds only what the link resolves globally. Without
     * them a `static' function has no name in the output at all: `nm', a
     * profiler and a crash dump can say nothing about it, where the platform's
     * linker names it. Debug info is a separate matter and was never affected.
     *
     * Only named symbols defined in a section that made it into the output.
     * SECTION and FILE entries are bookkeeping about the inputs and are left
     * out; HP's linker emits them, and nothing here needs them.
     */
    {
        size_t oi;
        for (oi = 0; oi < L->nobjs; oi++) {
            hld_elf *e = L->objs[oi];
            uint32_t j;

            for (j = 1; j < e->eh.shnum; j++) {
                hld_sym *syms;
                size_t n, k;
                char serr[HLD_ERRSZ];

                if (e->shdrs[j].type != SHT_SYMTAB) continue;
                syms = hld_read_syms(e, &e->shdrs[j], &n, serr);
                if (!syms) continue;      /* unreadable: not worth failing over */

                for (k = 1; k < n; k++) {
                    hld_sym *s = &syms[k];
                    uint8_t type = ELF64_ST_TYPE(s->info);
                    isec *in;

                    if (ELF64_ST_BIND(s->info) != STB_LOCAL) continue;
                    if (!s->name || !s->name[0]) continue;
                    if (type == STT_SECTION || type == STT_FILE) continue;
                    if (s->shndx == SHN_UNDEF || s->shndx >= e->eh.shnum)
                        continue;
                    in = hld_isec_of(L, e, s->shndx);
                    if (!in || !in->out) continue;   /* section not emitted */

                    if (nsym == symcap) {
                        uint8_t *nb = realloc(symbuf, (size_t)(symcap * 2 * symsz1));
                        if (!nb) { free(syms); goto oom; }
                        memset(nb + symcap * symsz1, 0,
                               (size_t)(symcap * symsz1));
                        symbuf = nb;
                        symcap *= 2;
                    }
                    put_sym(e32, symbuf + nsym * symsz1,
                            strtab_add(&str, s->name), s->info, s->other,
                            (uint16_t)in->out->shndx,
                            in->out->addr + in->out_off + s->value, s->size);
                    nsym++;
                }
                free(syms);
            }
        }
    }
    nlocal = (uint32_t)nsym;

    for (h = 0; h < HLD_SYMHASH; h++) {
        for (g = L->hash[h]; g; g = g->next) {
            uint16_t shndx = SHN_ABS;
            uint32_t nameoff;
            uint8_t info;

            if (g->kind != HLD_SYM_DEFINED && g->kind != HLD_SYM_ABS) continue;
            if (nsym == symcap) {
                uint8_t *nb = realloc(symbuf, (size_t)(symcap * 2 * symsz1));
                if (!nb) goto oom;
                memset(nb + symcap * symsz1, 0, (size_t)(symcap * symsz1));
                symbuf = nb;
                symcap *= 2;
            }
            if (g->kind == HLD_SYM_DEFINED && g->in)
                shndx = (uint16_t)g->in->out->shndx;
            nameoff = strtab_add(&str, g->name);
            info = ELF64_ST_INFO(g->bind ? g->bind : STB_GLOBAL, g->type);
            put_sym(e32, symbuf + nsym * symsz1, nameoff, info, g->other,
                    shndx, g->value, g->size);
            nsym++;
        }
    }
    symsz = nsym * symsz1;

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
            shtab = calloc(nsec, (size_t)shsz);
            if (!shtab) { free(names); goto oom; }

            k = 0;
            for (o = L->osecs; o; o = o->next, k++) {
                uint32_t link = 0, info = 0;
                /* .dynsym/.dynamic/.hash name the dynamic string/symbol table */
                if (L->dynamic && L->dynstrsec
                    && (o == L->dynsymsec || o == L->dynamicsec))
                    link = L->dynstrsec->shndx;
                if (L->dynamic && L->dynsymsec
                    && (o == L->hashsec || o == L->reladynsec))
                    link = L->dynsymsec->shndx;
                if (L->dynamic && L->pltsec && o == L->reladynsec)
                    info = L->pltsec->shndx;
                /*
                 * .dynsym's sh_info is the index of the first global. A
                 * shared library puts its segment anchors before them, and a
                 * reader that trusts sh_info would otherwise treat those
                 * locals as exports.
                 */
                if (L->dynamic && o == L->dynsymsec)
                    info = L->ndynlocal ? L->ndynlocal : 1;
                put_shdr(e32, shtab + o->shndx * shsz, names[k], o->type,
                         o->flags, o->addr, o->off, o->size, link, info,
                         o->align, o->entsize);
            }
            put_shdr(e32, shtab + sym_ndx * shsz, n_sym, SHT_SYMTAB, 0, 0,
                     symoff, symsz, str_ndx, nlocal, 8, symsz1);
            put_shdr(e32, shtab + str_ndx * shsz, n_str, SHT_STRTAB, 0, 0,
                     stroff, str.len, 0, 0, 1, 0);
            put_shdr(e32, shtab + shstr_ndx * shsz, n_shstr, SHT_STRTAB, 0, 0,
                     shstroff, shstr.len, 0, 0, 1, 0);
            free(names);

            if (img_write(&im, shoff, shtab, (size_t)nsec * (size_t)shsz) < 0) {
                free(shtab);
                goto oom;
            }
            free(shtab);
        }

        /*
         * Program headers. PT_INTERP must precede the loadable segments (the
         * gABI requires it, and the loader rejects an image otherwise), and
         * the platform's own linker orders them PHDR, INTERP, DYNAMIC, LOAD.
         */
        {
            uint32_t pi = 0;
#define PUT_PH(t, fl, of, va, fs, ms, al) do { \
                put_phdr(e32, ph, (t), (fl), (of), (va), (fs), (ms), (al)); \
                if (img_write(&im, phoff + pi * phsz, ph, \
                              (size_t)phsz) < 0) goto oom; \
                pi++; \
            } while (0)

            PUT_PH(PT_PHDR, PF_R, phoff, HLD_TEXTBASE(L) + phoff,
                   (uint64_t)nphdr * phsz,
                   (uint64_t)nphdr * phsz, 8);
            if (L->dynamic) {
                /* A library is not started by the kernel: no interpreter. */
                if (L->interpsec)
                    PUT_PH(PT_INTERP, PF_R, L->interpsec->off,
                           L->interpsec->addr, L->interpsec->size,
                           L->interpsec->size, 1);
                PUT_PH(PT_DYNAMIC, PF_R, L->dynamicsec->off,
                       L->dynamicsec->addr, L->dynamicsec->size,
                       L->dynamicsec->size, 8);
            }
            PUT_PH(PT_LOAD,
                   PF_R | PF_X | PF_HP_CODE | PF_HP_LAZYSWAP | PF_HP_UNNAMED17,
                   0, HLD_TEXTBASE(L), L->text_filesz, L->text_filesz, 0x10);
            PUT_PH(PT_LOAD, PF_R | PF_W | PF_HP_MODIFY, L->data_off,
                   L->data_addr, L->data_filesz, L->data_memsz, 0x10);
            if (L->has_tls)
                PUT_PH(PT_TLS, PF_R, L->tls_off, L->tls_base,
                       L->tls_filesz, L->tls_memsz, 0x10);
            if (L->unwind_sec) {
                osec *uh = L->unwind_hdr_sec ? L->unwind_hdr_sec : L->unwind_sec;
                osec *ue = L->unwind_info_sec ? L->unwind_info_sec : L->unwind_sec;
                uint64_t usz = ue->addr + ue->size - uh->addr;
                PUT_PH(PT_IA_64_UNWIND, PF_R, uh->off, uh->addr, usz, usz, 8);
            }
#undef PUT_PH
        }

        /* --- ELF header -------------------------------------------------- */
        memset(eh, 0, sizeof eh);
        memcpy(eh, ELFMAG, 4);
        eh[4] = e32 ? ELFCLASS32 : ELFCLASS64;
        eh[5] = ELFDATA2MSB;
        eh[6] = EV_CURRENT;
        eh[7] = ELFOSABI_HPUX;
        eh[8] = HPUX_ABIVERSION;
        st16(eh + 16, L->shared ? ET_DYN : ET_EXEC);
        st16(eh + 18, EM_IA_64);
        st32(eh + 20, EV_CURRENT);
        if (e32) {
            /* EF_IA_64_ABI64 says 64-bit ABI; an ILP32 image must not set it. */
            st32(eh + 24, (uint32_t)L->entry);
            st32(eh + 28, (uint32_t)phoff);
            st32(eh + 32, (uint32_t)shoff);
            st32(eh + 36, EF_IA_64_BE
                          | (L->trapnil ? EF_IA_64_TRAPNIL : 0));
            st16(eh + 40, (uint16_t)ehsz);
            st16(eh + 42, (uint16_t)phsz);
            st16(eh + 44, (uint16_t)nphdr);
            st16(eh + 46, (uint16_t)shsz);
            st16(eh + 48, (uint16_t)nsec);
            st16(eh + 50, (uint16_t)shstr_ndx);
        } else {
            st64(eh + 24, L->entry);
            st64(eh + 32, phoff);
            st64(eh + 40, shoff);
            st32(eh + 48, EF_IA_64_BE | EF_IA_64_ABI64
                          | (L->trapnil ? EF_IA_64_TRAPNIL : 0));
            st16(eh + 52, (uint16_t)ehsz);
            st16(eh + 54, (uint16_t)phsz);
            st16(eh + 56, (uint16_t)nphdr);
            st16(eh + 58, (uint16_t)shsz);
            st16(eh + 60, (uint16_t)nsec);
            st16(eh + 62, (uint16_t)shstr_ndx);
        }
        if (img_write(&im, 0, eh, (size_t)ehsz) < 0) goto oom;
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
    /* A linked program has to be runnable; honor the umask like any tool. */
    if (chmod(L->out_path, 0755) != 0) {
        snprintf(L->err, HLD_ERRSZ, "%s: cannot make the output executable",
                 L->out_path);
        goto done;
    }
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
