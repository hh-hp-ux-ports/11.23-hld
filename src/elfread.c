/*
 * elfread.c — loading and parsing of ELF64-MSB (ia64-hpux) files.
 *
 * This is the input layer of hld. It must never crash on malformed input:
 * every offset/size derived from file contents is bounds-checked against the
 * loaded image before use.
 */
#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf64.h"

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static void seterr(char *err, const char *fmt, const char *a, const char *b)
{
    if (!err) return;
    /* fmt uses at most two %s */
    snprintf(err, HLD_ERRSZ, fmt, a ? a : "", b ? b : "");
}

static int in_file(const hld_elf *e, uint64_t off, uint64_t len)
{
    return off <= e->size && len <= e->size - off;
}

static uint8_t *read_whole_file(const char *path, size_t *size, char *err)
{
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *buf;

    if (!f) { seterr(err, "%s: cannot open", path, NULL); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        seterr(err, "%s: cannot size", path, NULL);
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc(sz ? (size_t)sz : 1);
    if (!buf) { seterr(err, "%s: out of memory", path, NULL); fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        seterr(err, "%s: short read", path, NULL);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)sz;
    return buf;
}

/*
 * Parse an image already in memory. When `owns_data` is zero the buffer
 * belongs to the caller and outlives this object — that is how archive
 * members are read, borrowing the archive's own buffer rather than copying.
 */
hld_elf *hld_elf_from_memory(const char *name, uint8_t *data, size_t size,
                             int owns_data, char *err)
{
    hld_elf *e;
    const uint8_t *p;
    uint32_t i;
    const char *path = name;

    e = calloc(1, sizeof *e);
    if (!e) {
        seterr(err, "out of memory", NULL, NULL);
        if (owns_data) free(data);
        return NULL;
    }
    e->path = xstrdup(name);
    e->data = data;
    e->size = size;
    e->owns_data = owns_data;
    if (!e->path) goto fail_noerr;

    if (e->size < EHDR64_SIZE || memcmp(e->data, ELFMAG, 4) != 0) {
        seterr(err, "%s: not an ELF file", path, NULL);
        goto fail_noerr;
    }
    p = e->data;
    e->eh.cls    = p[4];
    e->eh.data   = p[5];
    e->eh.osabi  = p[7];
    e->eh.abiver = p[8];
    if (e->eh.cls != ELFCLASS64) {
        seterr(err, "%s: ELF32 input — hld links LP64 only, and nothing else "
                    "is supported yet", path, NULL);
        goto fail_noerr;
    }
    if (e->eh.data != ELFDATA2MSB) {
        seterr(err, "%s: little-endian input — hld links big-endian HP-UX/IPF "
                    "objects only", path, NULL);
        goto fail_noerr;
    }
    e->eh.type      = be16(p + 16);
    e->eh.machine   = be16(p + 18);
    e->eh.entry     = be64(p + 24);
    e->eh.phoff     = be64(p + 32);
    e->eh.shoff     = be64(p + 40);
    e->eh.flags     = be32(p + 48);
    e->eh.phentsize = be16(p + 54);
    e->eh.phnum     = be16(p + 56);
    e->eh.shentsize = be16(p + 58);
    e->eh.shnum     = be16(p + 60);
    e->eh.shstrndx  = be16(p + 62);

    if (e->eh.phnum) {
        if (e->eh.phentsize != PHDR64_SIZE
            || !in_file(e, e->eh.phoff, (uint64_t)e->eh.phnum * PHDR64_SIZE)) {
            seterr(err, "%s: program header table out of bounds", path, NULL);
            goto fail_noerr;
        }
        e->phdrs = calloc(e->eh.phnum, sizeof *e->phdrs);
        if (!e->phdrs) { seterr(err, "out of memory", NULL, NULL); goto fail_noerr; }
        for (i = 0; i < e->eh.phnum; i++) {
            const uint8_t *q = e->data + e->eh.phoff + (uint64_t)i * PHDR64_SIZE;
            hld_phdr *ph = &e->phdrs[i];
            ph->type   = be32(q + 0);
            ph->flags  = be32(q + 4);
            ph->offset = be64(q + 8);
            ph->vaddr  = be64(q + 16);
            ph->paddr  = be64(q + 24);
            ph->filesz = be64(q + 32);
            ph->memsz  = be64(q + 40);
            ph->align  = be64(q + 48);
        }
    }

    if (e->eh.shnum) {
        if (e->eh.shentsize != SHDR64_SIZE
            || !in_file(e, e->eh.shoff, (uint64_t)e->eh.shnum * SHDR64_SIZE)) {
            seterr(err, "%s: section header table out of bounds", path, NULL);
            goto fail_noerr;
        }
        e->shdrs = calloc(e->eh.shnum, sizeof *e->shdrs);
        if (!e->shdrs) { seterr(err, "out of memory", NULL, NULL); goto fail_noerr; }
        for (i = 0; i < e->eh.shnum; i++) {
            const uint8_t *q = e->data + e->eh.shoff + (uint64_t)i * SHDR64_SIZE;
            hld_shdr *sh = &e->shdrs[i];
            sh->name_off  = be32(q + 0);
            sh->type      = be32(q + 4);
            sh->flags     = be64(q + 8);
            sh->addr      = be64(q + 16);
            sh->offset    = be64(q + 24);
            sh->size      = be64(q + 32);
            sh->link      = be32(q + 40);
            sh->info      = be32(q + 44);
            sh->addralign = be64(q + 48);
            sh->entsize   = be64(q + 56);
            sh->name      = "";
        }
        /* resolve names via shstrtab */
        if (e->eh.shstrndx < e->eh.shnum) {
            for (i = 0; i < e->eh.shnum; i++)
                e->shdrs[i].name =
                    hld_strtab_str(e, e->eh.shstrndx, e->shdrs[i].name_off);
        }
    }
    return e;

fail_noerr:
    hld_elf_free(e);
    return NULL;
}

hld_elf *hld_elf_load(const char *path, char *err)
{
    uint8_t *data;
    size_t size;

    data = read_whole_file(path, &size, err);
    if (!data) return NULL;
    return hld_elf_from_memory(path, data, size, 1, err);
}

void hld_elf_free(hld_elf *e)
{
    if (!e) return;
    free(e->path);
    if (e->owns_data) free(e->data);
    free(e->shdrs);
    free(e->phdrs);
    free(e);
}

const uint8_t *hld_sec_data(const hld_elf *e, const hld_shdr *sh, char *err)
{
    if (sh->type == SHT_NOBITS) {
        seterr(err, "%s: section %s has no file data", e->path, sh->name);
        return NULL;
    }
    if (!in_file(e, sh->offset, sh->size)) {
        seterr(err, "%s: section %s data out of bounds", e->path, sh->name);
        return NULL;
    }
    return e->data + sh->offset;
}

const char *hld_strtab_str(const hld_elf *e, uint32_t strtab_ndx, uint64_t off)
{
    const hld_shdr *st;
    const uint8_t *base;

    if (strtab_ndx >= e->eh.shnum) return "";
    st = &e->shdrs[strtab_ndx];
    if (off >= st->size || !in_file(e, st->offset, st->size)) return "";
    base = e->data + st->offset;
    /* Guarantee NUL-termination inside the section. */
    if (!memchr(base + off, 0, st->size - off)) return "";
    return (const char *)(base + off);
}

hld_sym *hld_read_syms(const hld_elf *e, const hld_shdr *sh, size_t *count, char *err)
{
    const uint8_t *base = hld_sec_data(e, sh, err);
    hld_sym *out;
    size_t n, i;

    *count = 0;
    if (!base) return NULL;
    if (sh->entsize != SYM64_SIZE || sh->size % SYM64_SIZE) {
        seterr(err, "%s: %s: bad symbol entry size", e->path, sh->name);
        return NULL;
    }
    n = sh->size / SYM64_SIZE;
    out = calloc(n ? n : 1, sizeof *out);
    if (!out) { seterr(err, "out of memory", NULL, NULL); return NULL; }
    for (i = 0; i < n; i++) {
        const uint8_t *q = base + i * SYM64_SIZE;
        hld_sym *s = &out[i];
        s->name_off = be32(q + 0);
        s->info     = q[4];
        s->other    = q[5];
        s->shndx    = be16(q + 6);
        s->value    = be64(q + 8);
        s->size     = be64(q + 16);
        s->name     = hld_strtab_str(e, sh->link, s->name_off);
    }
    *count = n;
    return out;
}

hld_rela *hld_read_relas(const hld_elf *e, const hld_shdr *sh, size_t *count, char *err)
{
    const uint8_t *base = hld_sec_data(e, sh, err);
    hld_rela *out;
    size_t n, i;

    *count = 0;
    if (!base) return NULL;
    if (sh->entsize != RELA64_SIZE || sh->size % RELA64_SIZE) {
        seterr(err, "%s: %s: bad rela entry size", e->path, sh->name);
        return NULL;
    }
    n = sh->size / RELA64_SIZE;
    out = calloc(n ? n : 1, sizeof *out);
    if (!out) { seterr(err, "out of memory", NULL, NULL); return NULL; }
    for (i = 0; i < n; i++) {
        const uint8_t *q = base + i * RELA64_SIZE;
        uint64_t info = be64(q + 8);
        out[i].offset = be64(q + 0);
        out[i].sym    = ELF64_R_SYM(info);
        out[i].type   = ELF64_R_TYPE(info);
        out[i].addend = (int64_t)be64(q + 16);
    }
    *count = n;
    return out;
}

hld_dyn *hld_read_dyn(const hld_elf *e, const hld_shdr *sh, size_t *count, char *err)
{
    const uint8_t *base = hld_sec_data(e, sh, err);
    hld_dyn *out;
    size_t n, i;

    *count = 0;
    if (!base) return NULL;
    if (sh->size % DYN64_SIZE) {
        seterr(err, "%s: %s: bad dynamic section size", e->path, sh->name);
        return NULL;
    }
    n = sh->size / DYN64_SIZE;
    out = calloc(n ? n : 1, sizeof *out);
    if (!out) { seterr(err, "out of memory", NULL, NULL); return NULL; }
    for (i = 0; i < n; i++) {
        const uint8_t *q = base + i * DYN64_SIZE;
        out[i].tag = (int64_t)be64(q + 0);
        out[i].val = be64(q + 8);
    }
    *count = n;
    return out;
}

const hld_shdr *hld_sec_by_addr(const hld_elf *e, uint64_t addr)
{
    uint32_t i;
    for (i = 0; i < e->eh.shnum; i++) {
        const hld_shdr *sh = &e->shdrs[i];
        if (!(sh->flags & SHF_ALLOC)) continue;
        if (addr >= sh->addr && addr - sh->addr < (sh->size ? sh->size : 1))
            return sh;
    }
    return NULL;
}

/* ---- name tables ------------------------------------------------------- */

const char *hld_reloc_name(uint32_t type)
{
    switch (type) {
#define X(n, v) case v: return #n;
    HLD_IA64_RELOC_LIST(X)
#undef X
    default: return NULL;
    }
}

const char *hld_sht_name(uint32_t type)
{
    switch (type) {
    case SHT_NULL:          return "NULL";
    case SHT_PROGBITS:      return "PROGBITS";
    case SHT_SYMTAB:        return "SYMTAB";
    case SHT_STRTAB:        return "STRTAB";
    case SHT_RELA:          return "RELA";
    case SHT_HASH:          return "HASH";
    case SHT_DYNAMIC:       return "DYNAMIC";
    case SHT_NOTE:          return "NOTE";
    case SHT_NOBITS:        return "NOBITS";
    case SHT_REL:           return "REL";
    case SHT_SHLIB:         return "SHLIB";
    case SHT_DYNSYM:        return "DYNSYM";
    case SHT_INIT_ARRAY:    return "INIT_ARRAY";
    case SHT_FINI_ARRAY:    return "FINI_ARRAY";
    case SHT_PREINIT_ARRAY: return "PREINIT_ARRAY";
    case SHT_GROUP:         return "GROUP";
    case SHT_SYMTAB_SHNDX:  return "SYMTAB_SHNDX";
    case SHT_HP_OPT_ANNOT:  return "HP_OPT_ANNOT";
    case SHT_IA_64_EXT:     return "IA_64_EXT";
    case SHT_IA_64_UNWIND:  return "IA_64_UNWIND";
    default:                return NULL;
    }
}

const char *hld_pt_name(uint32_t type)
{
    switch (type) {
    case PT_NULL:    return "NULL";
    case PT_LOAD:    return "LOAD";
    case PT_DYNAMIC: return "DYNAMIC";
    case PT_INTERP:  return "INTERP";
    case PT_NOTE:    return "NOTE";
    case PT_SHLIB:   return "SHLIB";
    case PT_PHDR:    return "PHDR";
    case PT_TLS:     return "TLS";
    case PT_HP_TLS:              return "HP_TLS";
    case PT_HP_CORE_NONE:        return "HP_CORE_NONE";
    case PT_HP_CORE_VERSION:     return "HP_CORE_VERSION";
    case PT_HP_CORE_KERNEL:      return "HP_CORE_KERNEL";
    case PT_HP_CORE_COMM:        return "HP_CORE_COMM";
    case PT_HP_CORE_PROC:        return "HP_CORE_PROC";
    case PT_HP_CORE_LOADABLE:    return "HP_CORE_LOADABLE";
    case PT_HP_CORE_STACK:       return "HP_CORE_STACK";
    case PT_HP_CORE_SHM:         return "HP_CORE_SHM";
    case PT_HP_CORE_MMF:         return "HP_CORE_MMF";
    case PT_HP_PARALLEL:         return "HP_PARALLEL";
    case PT_HP_FASTBIND:         return "HP_FASTBIND";
    case PT_HP_OPT_ANNOT:        return "HP_OPT_ANNOT";
    case PT_HP_HSL_ANNOT:        return "HP_HSL_ANNOT";
    case PT_HP_STACK:            return "HP_STACK";
    case PT_HP_CORE_UTSNAME:     return "HP_CORE_UTSNAME";
    case PT_HP_LINKER_FOOTPRINT: return "HP_LINKER_FOOTPRINT";
    case PT_IA_64_ARCHEXT: return "IA_64_ARCHEXT";
    case PT_IA_64_UNWIND:  return "IA_64_UNWIND";
    default:         return NULL;
    }
}

const char *hld_dt_name(int64_t tag)
{
    switch (tag) {
    case DT_NULL:         return "NULL";
    case DT_NEEDED:       return "NEEDED";
    case DT_PLTRELSZ:     return "PLTRELSZ";
    case DT_PLTGOT:       return "PLTGOT";
    case DT_HASH:         return "HASH";
    case DT_STRTAB:       return "STRTAB";
    case DT_SYMTAB:       return "SYMTAB";
    case DT_RELA:         return "RELA";
    case DT_RELASZ:       return "RELASZ";
    case DT_RELAENT:      return "RELAENT";
    case DT_STRSZ:        return "STRSZ";
    case DT_SYMENT:       return "SYMENT";
    case DT_INIT:         return "INIT";
    case DT_FINI:         return "FINI";
    case DT_SONAME:       return "SONAME";
    case DT_RPATH:        return "RPATH";
    case DT_SYMBOLIC:     return "SYMBOLIC";
    case DT_REL:          return "REL";
    case DT_RELSZ:        return "RELSZ";
    case DT_RELENT:       return "RELENT";
    case DT_PLTREL:       return "PLTREL";
    case DT_DEBUG:        return "DEBUG";
    case DT_TEXTREL:      return "TEXTREL";
    case DT_JMPREL:       return "JMPREL";
    case DT_BIND_NOW:     return "BIND_NOW";
    case DT_INIT_ARRAY:   return "INIT_ARRAY";
    case DT_FINI_ARRAY:   return "FINI_ARRAY";
    case DT_INIT_ARRAYSZ: return "INIT_ARRAYSZ";
    case DT_FINI_ARRAYSZ: return "FINI_ARRAYSZ";
    case DT_RUNPATH:      return "RUNPATH";
    case DT_FLAGS:        return "FLAGS";
    case DT_PREINIT_ARRAY:   return "PREINIT_ARRAY";
    case DT_PREINIT_ARRAYSZ: return "PREINIT_ARRAYSZ";
    case DT_HP_LOAD_MAP:        return "HP_LOAD_MAP";
    case DT_HP_DLD_FLAGS:       return "HP_DLD_FLAGS";
    case DT_HP_DLD_HOOK:        return "HP_DLD_HOOK";
    case DT_HP_UX10_INIT:       return "HP_UX10_INIT";
    case DT_HP_UX10_INITSZ:     return "HP_UX10_INITSZ";
    case DT_HP_PREINIT:         return "HP_PREINIT";
    case DT_HP_PREINITSZ:       return "HP_PREINITSZ";
    case DT_HP_NEEDED:          return "HP_NEEDED";
    case DT_HP_TIME_STAMP:      return "HP_TIME_STAMP";
    case DT_HP_CHECKSUM:        return "HP_CHECKSUM";
    case DT_HP_GST_SIZE:        return "HP_GST_SIZE";
    case DT_HP_GST_VERSION:     return "HP_GST_VERSION";
    case DT_HP_GST_HASHVAL:     return "HP_GST_HASHVAL";
    case DT_HP_EPLTREL:         return "HP_EPLTREL";
    case DT_HP_EPLTRELSZ:       return "HP_EPLTRELSZ";
    case DT_HP_FILTERED:        return "HP_FILTERED";
    case DT_HP_FILTER_TLS:      return "HP_FILTER_TLS";
    case DT_HP_COMPAT_FILTERED: return "HP_COMPAT_FILTERED";
    case DT_HP_LAZYLOAD:        return "HP_LAZYLOAD";
    case DT_HP_BIND_NOW_COUNT:  return "HP_BIND_NOW_COUNT";
    case DT_IA_64_PLT_RESERVE:  return "IA_64_PLT_RESERVE";
    default:              return NULL;
    }
}
