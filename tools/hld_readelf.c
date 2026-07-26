/*
 * hld-readelf — dump ELF64-MSB ia64-hpux files using hld's own reader.
 *
 * Exists to validate the reader against real artifacts (cross-check with GNU
 * readelf) and as the debugging eye for hld's own outputs later. Output is
 * GNU-readelf-flavored but not byte-compatible.
 */
#include "../src/port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/elf64.h"

#define U(x)  ((unsigned long long)(x))
#define S(x)  ((long long)(x))

static int opt_h, opt_S, opt_l, opt_s, opt_r, opt_d;

static const char *osabi_name(uint8_t v)
{
    switch (v) {
    case 0: return "SysV";
    case ELFOSABI_HPUX: return "HP-UX";
    default: return "?";
    }
}

static const char *etype_name(uint16_t t)
{
    switch (t) {
    case ET_REL: return "REL (relocatable)";
    case ET_EXEC: return "EXEC (executable)";
    case ET_DYN: return "DYN (shared object)";
    default: return "?";
    }
}

static void print_ehdr(const hld_elf *e)
{
    const hld_ehdr *h = &e->eh;
    char fl[64];

    fl[0] = 0;
    if (h->machine == EM_IA_64) {
        if (h->flags & EF_IA_64_TRAPNIL) strcat(fl, " TRAPNIL");
        if (h->flags & EF_IA_64_BE)      strcat(fl, " BE");
        if (h->flags & EF_IA_64_ABI64)   strcat(fl, " ABI64");
    }
    printf("ELF header:\n");
    printf("  Class/Data:  ELF%d, %s\n", h->cls == ELFCLASS64 ? 64 : 32,
           h->data == ELFDATA2MSB ? "big-endian" : "little-endian");
    printf("  OS/ABI:      %s (%u), ABI version %u\n",
           osabi_name(h->osabi), h->osabi, h->abiver);
    printf("  Type:        %s\n", etype_name(h->type));
    printf("  Machine:     %s (%u)\n",
           h->machine == EM_IA_64 ? "IA-64" : "?", h->machine);
    printf("  Entry:       0x%llx\n", U(h->entry));
    printf("  Flags:       0x%x%s\n", h->flags, fl);
    printf("  Phdrs:       %u @ 0x%llx\n", h->phnum, U(h->phoff));
    printf("  Shdrs:       %u @ 0x%llx (shstrndx %u)\n",
           h->shnum, U(h->shoff), h->shstrndx);
}

static void secflags_str(uint64_t f, char *out /* >=16 */)
{
    char *p = out;
    if (f & SHF_WRITE)         *p++ = 'W';
    if (f & SHF_ALLOC)         *p++ = 'A';
    if (f & SHF_EXECINSTR)     *p++ = 'X';
    if (f & SHF_MERGE)         *p++ = 'M';
    if (f & SHF_STRINGS)       *p++ = 'S';
    if (f & SHF_INFO_LINK)     *p++ = 'I';
    if (f & SHF_LINK_ORDER)    *p++ = 'L';
    if (f & SHF_GROUP)         *p++ = 'G';
    if (f & SHF_TLS)           *p++ = 'T';
    if (f & SHF_IA_64_SHORT)   *p++ = 'p';
    *p = 0;
}

static void print_shdrs(const hld_elf *e)
{
    uint32_t i;

    printf("Section headers (%u):\n", e->eh.shnum);
    printf("  [Nr] %-22s %-14s %-16s %-8s %-8s ES Flg Lk Inf Al\n",
           "Name", "Type", "Addr", "Off", "Size");
    for (i = 0; i < e->eh.shnum; i++) {
        const hld_shdr *sh = &e->shdrs[i];
        const char *tn = hld_sht_name(sh->type);
        char tbuf[24], flg[16];

        if (!tn) {
            snprintf(tbuf, sizeof tbuf, "0x%x", sh->type);
            tn = tbuf;
        }
        secflags_str(sh->flags, flg);
        printf("  [%2u] %-22s %-14s %016llx %08llx %08llx %02llx %-3s %2u %3u %2llu\n",
               i, sh->name, tn, U(sh->addr), U(sh->offset), U(sh->size),
               U(sh->entsize), flg, sh->link, sh->info, U(sh->addralign));
    }
}

static void print_phdrs(const hld_elf *e)
{
    uint32_t i, j;

    printf("Program headers (%u):\n", e->eh.phnum);
    printf("  %-20s %-8s %-16s %-8s %-8s Flg Align\n",
           "Type", "Offset", "VirtAddr", "FileSiz", "MemSiz");
    for (i = 0; i < e->eh.phnum; i++) {
        const hld_phdr *ph = &e->phdrs[i];
        const char *tn = hld_pt_name(ph->type);
        char tbuf[24], flg[4];

        if (!tn) {
            snprintf(tbuf, sizeof tbuf, "0x%x", ph->type);
            tn = tbuf;
        }
        flg[0] = (char)((ph->flags & PF_R) ? 'R' : ' ');
        flg[1] = (char)((ph->flags & PF_W) ? 'W' : ' ');
        flg[2] = (char)((ph->flags & PF_X) ? 'E' : ' ');
        flg[3] = 0;
        printf("  %-20s %08llx %016llx %08llx %08llx %s 0x%llx\n",
               tn, U(ph->offset), U(ph->vaddr), U(ph->filesz), U(ph->memsz),
               flg, U(ph->align));
        if (ph->type == PT_INTERP) {
            char err[HLD_ERRSZ];
            uint32_t k;
            /* interp string = bytes at the segment's file offset */
            for (k = 0; k < e->eh.shnum; k++) {
                const hld_shdr *sh = &e->shdrs[k];
                if (sh->offset == ph->offset && sh->size == ph->filesz
                    && hld_sec_data(e, sh, err)) {
                    printf("      [interpreter: %.*s]\n",
                           (int)sh->size, (const char *)hld_sec_data(e, sh, err));
                    break;
                }
            }
        }
    }
    /* section-to-segment mapping (alloc sections by vaddr; others by offset
     * for non-LOAD segments — a debugging aid, not gABI-exact) */
    printf("  Section→segment:\n");
    for (i = 0; i < e->eh.phnum; i++) {
        const hld_phdr *ph = &e->phdrs[i];
        printf("   %02u: ", i);
        for (j = 0; j < e->eh.shnum; j++) {
            const hld_shdr *sh = &e->shdrs[j];
            int in = 0;
            if (sh->type == SHT_NULL) continue;
            if (sh->flags & SHF_ALLOC) {
                uint64_t end = ph->vaddr + ph->memsz;
                in = sh->addr >= ph->vaddr
                     && sh->addr + sh->size <= end
                     && (sh->size || sh->addr < end || ph->memsz == 0);
            } else if (ph->type != PT_LOAD && ph->vaddr == 0 && ph->filesz) {
                in = sh->offset >= ph->offset
                     && sh->offset + (sh->type == SHT_NOBITS ? 0 : sh->size)
                        <= ph->offset + ph->filesz
                     && sh->size;
            }
            if (in) printf("%s ", sh->name);
        }
        printf("\n");
    }
}

static void print_syms_of(const hld_elf *e, const hld_shdr *sh)
{
    static const char *binds[] = { "LOCAL", "GLOBAL", "WEAK" };
    static const char *types[] = { "NOTYPE", "OBJECT", "FUNC", "SECTION",
                                   "FILE", "COMMON", "TLS" };
    static const char *vis[]   = { "DEFAULT", "INTERNAL", "HIDDEN", "PROTECTED" };
    char err[HLD_ERRSZ];
    size_t n, i;
    hld_sym *syms = hld_read_syms(e, sh, &n, err);

    if (!syms) { printf("  (unreadable: %s)\n", err); return; }
    printf("Symbol table '%s' (%lu entries):\n", sh->name, (unsigned long)n);
    printf("  %5s %-16s %6s %-7s %-6s %-9s %-4s Name\n",
           "Num", "Value", "Size", "Type", "Bind", "Vis", "Ndx");
    for (i = 0; i < n; i++) {
        const hld_sym *s = &syms[i];
        uint8_t b = ELF64_ST_BIND(s->info), t = ELF64_ST_TYPE(s->info);
        uint8_t v = ELF64_ST_VISIBILITY(s->other);
        char bbuf[8], tbuf[8], nbuf[8];
        const char *bn = b < 3 ? binds[b] : (snprintf(bbuf, 8, "%u", b), bbuf);
        const char *tn = t < 7 ? types[t] : (snprintf(tbuf, 8, "%u", t), tbuf);
        const char *nx;

        if (s->shndx == SHN_UNDEF)       nx = "UND";
        else if (s->shndx == SHN_ABS)    nx = "ABS";
        else if (s->shndx == SHN_COMMON) nx = "COM";
        else { snprintf(nbuf, 8, "%u", s->shndx); nx = nbuf; }
        printf("  %5lu %016llx %6llu %-7s %-6s %-9s %-4s %s\n",
               (unsigned long)i, U(s->value), U(s->size), tn, bn, vis[v], nx,
               s->name);
    }
    free(syms);
}

static void print_relas_of(const hld_elf *e, const hld_shdr *sh)
{
    char err[HLD_ERRSZ];
    size_t n, i, nsyms = 0;
    hld_rela *rel = hld_read_relas(e, sh, &n, err);
    hld_sym *syms = NULL;

    if (!rel) { printf("  (unreadable: %s)\n", err); return; }
    if (sh->link < e->eh.shnum
        && (e->shdrs[sh->link].type == SHT_SYMTAB
            || e->shdrs[sh->link].type == SHT_DYNSYM))
        syms = hld_read_syms(e, &e->shdrs[sh->link], &nsyms, err);
    printf("Relocation section '%s' (%lu entries):\n", sh->name,
           (unsigned long)n);
    printf("  %-16s %-22s %-16s Sym + Addend\n", "Offset", "Type", "SymValue");
    for (i = 0; i < n; i++) {
        const hld_rela *r = &rel[i];
        const char *tn = hld_reloc_name(r->type);
        const char *sn = "";
        uint64_t sv = 0;
        char tbuf[24];

        if (!tn) { snprintf(tbuf, sizeof tbuf, "0x%x", r->type); tn = tbuf; }
        if (syms && r->sym < nsyms) {
            sn = syms[r->sym].name;
            sv = syms[r->sym].value;
            if (!sn[0] && ELF64_ST_TYPE(syms[r->sym].info) == STT_SECTION
                && syms[r->sym].shndx < e->eh.shnum)
                sn = e->shdrs[syms[r->sym].shndx].name;
        }
        printf("  %016llx %-22s %016llx %s + %llx\n",
               U(r->offset), tn, U(sv), sn, U((uint64_t)r->addend));
    }
    free(syms);
    free(rel);
}

static void print_dynamic_of(const hld_elf *e, const hld_shdr *sh)
{
    char err[HLD_ERRSZ];
    size_t n, i;
    hld_dyn *dyn = hld_read_dyn(e, sh, &n, err);
    uint64_t straddr = 0;

    if (!dyn) { printf("  (unreadable: %s)\n", err); return; }
    for (i = 0; i < n; i++)
        if (dyn[i].tag == DT_STRTAB) straddr = dyn[i].val;
    printf("Dynamic section '%s' (%lu entries):\n", sh->name, (unsigned long)n);
    for (i = 0; i < n; i++) {
        const hld_dyn *d = &dyn[i];
        const char *tn = hld_dt_name(d->tag);
        char tbuf[24];

        if (!tn) { snprintf(tbuf, sizeof tbuf, "0x%llx", U(d->tag)); tn = tbuf; }
        printf("  %-20s 0x%llx", tn, U(d->val));
        if ((d->tag == DT_NEEDED || d->tag == DT_SONAME || d->tag == DT_RPATH
             || d->tag == DT_RUNPATH) && straddr) {
            const hld_shdr *st = hld_sec_by_addr(e, straddr);
            if (st && st->type == SHT_STRTAB) {
                uint32_t ndx = (uint32_t)(st - e->shdrs);
                printf("  [%s]", hld_strtab_str(e, ndx, d->val));
            }
        }
        printf("\n");
        if (d->tag == DT_NULL) break;
    }
    free(dyn);
}

static int dump_file(const char *path)
{
    char err[HLD_ERRSZ];
    hld_elf *e = hld_elf_load(path, err);
    uint32_t i;

    if (!e) {
        fprintf(stderr, "hld-readelf: %s\n", err);
        return 1;
    }
    printf("\nFile: %s\n", path);
    if (opt_h) print_ehdr(e);
    if (opt_S) print_shdrs(e);
    if (opt_l && e->eh.phnum) print_phdrs(e);
    if (opt_d)
        for (i = 0; i < e->eh.shnum; i++)
            if (e->shdrs[i].type == SHT_DYNAMIC)
                print_dynamic_of(e, &e->shdrs[i]);
    if (opt_r)
        for (i = 0; i < e->eh.shnum; i++)
            if (e->shdrs[i].type == SHT_RELA)
                print_relas_of(e, &e->shdrs[i]);
    if (opt_s)
        for (i = 0; i < e->eh.shnum; i++)
            if (e->shdrs[i].type == SHT_SYMTAB
                || e->shdrs[i].type == SHT_DYNSYM)
                print_syms_of(e, &e->shdrs[i]);
    hld_elf_free(e);
    return 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0, any = 0, nfiles = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            int j;
            for (j = 1; a[j]; j++) {
                switch (a[j]) {
                case 'h': opt_h = 1; any = 1; break;
                case 'S': opt_S = 1; any = 1; break;
                case 'l': opt_l = 1; any = 1; break;
                case 's': opt_s = 1; any = 1; break;
                case 'r': opt_r = 1; any = 1; break;
                case 'd': opt_d = 1; any = 1; break;
                case 'a': opt_h = opt_S = opt_l = opt_s = opt_r = opt_d = 1;
                          any = 1; break;
                default:
                    fprintf(stderr,
                        "usage: hld-readelf [-hSlsrda] file...\n");
                    return 2;
                }
            }
        }
    }
    if (!any) opt_h = 1;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        nfiles++;
        rc |= dump_file(argv[i]);
    }
    if (!nfiles) {
        fprintf(stderr, "usage: hld-readelf [-hSlsrda] file...\n");
        return 2;
    }
    return rc;
}
