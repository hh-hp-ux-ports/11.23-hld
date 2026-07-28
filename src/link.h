/*
 * link.h — hld's internal link state: input pieces, output sections,
 * the symbol table, and the layout/emit entry points.
 */
#ifndef HLD_LINK_H
#define HLD_LINK_H

#include "elf64.h"

/* HP-UX/IPF LP64 address space (docs/format-notes.md). */
#define HLD_TEXT_BASE 0x4000000000000000ULL
#define HLD_DATA_BASE 0x6000000000000000ULL
#define HLD_SEG_ALIGN 0x10000ULL   /* file offset must be congruent to vaddr */

struct osec;

/* One input section contributing to an output section. */
typedef struct isec {
    hld_elf *obj;
    uint32_t idx;             /* section index within obj */
    const hld_shdr *sh;
    uint64_t out_off;         /* offset within the output section */
    struct osec *out;
    struct isec *next;        /* next contribution to the same output section */
} isec;

typedef struct osec {
    const char *name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr, off, size, align, entsize;
    uint32_t shndx;           /* index in the output section header table */
    uint8_t *data;            /* materialized contents (NULL for NOBITS) */
    isec *first, **tail;      /* contributions in link order */
    struct osec *next;
} osec;

typedef struct dsosym {
    const char *name;
    uint64_t value;
    struct dsosym *next;
} dsosym;

typedef struct hld_dso {
    hld_elf *elf;
    const char *soname;       /* DT_SONAME, or the file's basename */
    dsosym *hash[1021];
    int needed;               /* something actually resolved to it */
    uint32_t strx;            /* soname offset in .dynstr */
    struct hld_dso *next;
} hld_dso;

typedef enum {
    HLD_SYM_UNDEF,            /* referenced, not yet defined */
    HLD_SYM_IMPORT,           /* satisfied by a shared library */
    HLD_SYM_DEFINED,          /* defined by an input section */
    HLD_SYM_ABS,              /* absolute value (linker-defined or SHN_ABS) */
    HLD_SYM_COMMON            /* tentative definition, not yet allocated */
} hld_symkind;

typedef struct hld_gsym {
    const char *name;
    hld_symkind kind;
    uint64_t value;           /* final address (ABS: the value itself) */
    uint64_t size;
    uint8_t type, bind, other;
    int short_common;         /* COMMON belongs in the short bss (.sbss) */
    isec *in;                 /* defining input section (DEFINED) */
    uint64_t in_off;          /* offset within that input section */
    hld_elf *obj;             /* defining object, for diagnostics */
    hld_dso *dso;             /* IMPORT: the library that defines it */
    uint64_t hint;            /* IMPORT: its address there, a binding hint */
    uint64_t plt_slot;        /* IMPORT: byte offset within .plt */
    uint64_t stub_off;        /* IMPORT: byte offset within the stub section */
    uint32_t dynidx;          /* index in .dynsym */
    uint32_t strx;            /* name offset in .dynstr */
    struct hld_gsym *next;    /* hash chain */
} hld_gsym;

#define HLD_SYMHASH 1021
#define HLD_LNKHASH 1021
#define HLD_ARHASH  1021

/* One member of an `ar` archive, and the archive itself. */
typedef struct arsym {
    const char *name;         /* into the member's own string table */
    size_t member;
    struct arsym *next;
} arsym;

typedef struct armember {
    const char *name;
    size_t off, len;          /* within the archive's buffer */
    int extracted;
    hld_elf *elf;             /* parsed once, borrowing that buffer */
} armember;

typedef struct hld_archive {
    char *path;
    uint8_t *data;
    size_t size;
    armember *members;
    size_t nmembers;
    arsym *hash[HLD_ARHASH];  /* our own index: defined globals -> member */
    size_t nrejected;         /* members that could not be read */
    char reject[160];         /* why the first of them could not be */
    struct hld_archive *next;
} hld_archive;

/*
 * A linkage-table entry. Both the DLT (the GOT: 8-byte slots holding an
 * address, reached gp-relatively) and the descriptor table (.opd: 16-byte
 * {code address, gp} pairs) are keyed the same way — by the target the
 * relocation names, plus an addend.
 */
typedef struct lnkent {
    struct hld_gsym *g;       /* global target, or NULL for a local one */
    struct isec *in;          /* local target's input section */
    uint64_t off;             /* addend (global) or offset within `in` */
    int is_fptr;              /* DLT only: slot holds a descriptor's address */
    uint64_t slot;            /* byte offset within the DLT / .opd */
    struct lnkent *hnext;     /* hash chain */
    struct lnkent *next;      /* allocation order, for filling contents */
} lnkent;

typedef struct {
    /* inputs */
    hld_elf **objs;
    size_t nobjs, objs_cap;

    /* outputs */
    osec *osecs, **osec_tail;
    size_t nosecs;

    hld_gsym *hash[HLD_SYMHASH];

    /* linkage tables */
    lnkent *dlt_hash[HLD_LNKHASH], *dlt, **dlt_tail;
    lnkent *opd_hash[HLD_LNKHASH], *opd, **opd_tail;
    /*
     * Descriptors reached gp-relatively, for @pltoff. HP's compiler calls
     * external functions this way where gcc emits a direct branch, so an
     * object from either compiler links.
     */
    lnkent *pltoff_hash[HLD_LNKHASH], *pltoff, **pltoff_tail;
    uint64_t ndlt, nopd, npltoff;
    osec *dltsec, *opdsec, *pltoffsec;

    /* layout results */
    uint64_t text_addr, text_end, text_filesz;
    uint64_t data_addr, data_off, data_filesz, data_memsz;
    uint64_t entry;
    uint64_t gp;

    /* archives, in command-line order */
    hld_archive *archives, **ar_tail;
    size_t narchives;

    /* dynamic output */
    int dynamic;
    hld_dso *dsos, **dso_tail;
    size_t ndsos, nimports;
    osec *pltsec, *relapltsec, *stubsec;
    char **libpaths;
    size_t nlibpaths, libpaths_cap;
    char *dynstr;
    size_t dynstr_len, dynstr_cap;
    uint32_t ndynsym, nbucket, ndyntags, nphdr;
    uint64_t reserve_off, loadmap_off;
    osec *interpsec, *dynsymsec, *dynstrsec, *hashsec, *dynamicsec, *reservesec;

    /* options */
    const char *out_path;
    const char *entry_name;
    int trapnil;              /* -z */
    int map;                  /* -m */
    int verbose;

    char err[HLD_ERRSZ];
} hld_link;

/* link.c */
int  hld_add_object(hld_link *L, const char *path);
int  hld_input_object(hld_link *L, hld_elf *e);
int  hld_add_undefined(hld_link *L, const char *name);
int  hld_allocate_commons(hld_link *L);
int  hld_alloc_linkage(hld_link *L);
int  hld_layout(hld_link *L);
int  hld_build_contents(hld_link *L);
int  hld_relocate(hld_link *L);
void hld_print_map(hld_link *L);
void hld_link_free(hld_link *L);
hld_gsym *hld_sym_lookup(hld_link *L, const char *name);
osec *osec_get(hld_link *L, const char *name, uint32_t type, uint64_t flags);

/* archive.c */
int  hld_archive_open(hld_link *L, const char *path, hld_archive **out);
int  hld_archive_search(hld_link *L, hld_archive *ar, int *extracted_any);
void hld_archive_free(hld_archive *ar);
int  hld_link_millicode(hld_link *L);

/* dynamic.c */
int  hld_add_dso(hld_link *L, const char *path);
int  hld_add_libpath(hld_link *L, const char *dir);
int  hld_find_library(hld_link *L, const char *name, hld_archive **ar_out);
int  hld_predefine_symbols(hld_link *L);
int  hld_bind_imports(hld_link *L);
int  hld_alloc_dynamic(hld_link *L);
int  hld_fill_dynamic(hld_link *L);

/* write.c */
int  hld_write_exec(hld_link *L);

#endif /* HLD_LINK_H */
